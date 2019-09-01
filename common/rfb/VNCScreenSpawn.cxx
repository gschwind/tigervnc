/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2009-2019 Pierre Ossman for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

// -=- Single-Threaded VNC Server implementation


// Note about how sockets get closed:
//
// Closing sockets to clients is non-trivial because the code which calls
// VNCServerSpawn must explicitly know about all the sockets (so that it can block
// on them appropriately).  However, VNCServerSpawn may want to close clients for
// a number of reasons, and from a variety of entry points.  The simplest is
// when processSocketEvent() is called for a client, and the remote end has
// closed its socket.  A more complex reason is when processSocketEvent() is
// called for a client which has just sent a ClientInit with the shared flag
// set to false - in this case we want to close all other clients.  Yet another
// reason for disconnecting clients is when the desktop size has changed as a
// result of a call to setPixelBuffer().
//
// The responsibility for creating and deleting sockets is entirely with the
// calling code.  When VNCServerSpawn wants to close a connection to a client it
// calls the VNCSConnectionSpawnX's close() method which calls shutdown() on the
// socket.  Eventually the calling code will notice that the socket has been
// shut down and call removeSocket() so that we can delete the
// VNCSConnectionSpawnX.  Note that the socket must not be deleted by the calling
// code until after removeSocket() has been called.
//
// One minor complication is that we don't allocate a VNCSConnectionSpawnX object
// for a blacklisted host (since we want to minimise the resources used for
// dealing with such a connection).  In order to properly implement the
// getSockets function, we must maintain a separate closingSockets list,
// otherwise blacklisted connections might be "forgotten".


#include <assert.h>
#include <stdlib.h>

#include <rfb/ComparingUpdateTracker.h>
#include <rfb/KeyRemapper.h>
#include <rfb/LogWriter.h>
#include <rfb/Security.h>
#include <rfb/ServerCore.h>
#include <rfb/VNCScreenSpawn.h>
#include <rfb/VNCSConnectionSpawnX.h>
#include <rfb/util.h>
#include <rfb/ledStates.h>

#include <rdr/types.h>

using namespace rfb;

static LogWriter slog("VNCServerSpawn");
static LogWriter connectionsLog("Connections");

//
// -=- VNCServerSpawn Implementation
//

// -=- Constructors/Destructor

VNCScreenSpawn::VNCScreenSpawn(const char* name_)
  : blHosts(&blacklist), desktopStarted(false),
    blockCounter(0), pb(0), ledState(ledUnknown),
    name(strDup(name_)), pointerClient(0), clipboardClient(0),
    comparer(0), cursor(new Cursor(0, 0, Point(), NULL)),
    renderedCursorInvalid(false),
    keyRemapper(&KeyRemapper::defInstance),
    idleTimer(this), disconnectTimer(this), connectTimer(this),
    frameTimer(this)
{
  slog.debug("creating single-threaded server %s", name.buf);

  // FIXME: Do we really want to kick off these right away?
  if (rfb::Server::maxIdleTime)
    idleTimer.start(secsToMillis(rfb::Server::maxIdleTime));
  if (rfb::Server::maxDisconnectionTime)
    disconnectTimer.start(secsToMillis(rfb::Server::maxDisconnectionTime));
}

VNCScreenSpawn::~VNCScreenSpawn()
{
  slog.debug("shutting down server %s", name.buf);

  // Close any active clients, with appropriate logging & cleanup
  closeClients("Server shutdown");

  // Stop trying to render things
  stopFrameClock();

  // Delete all the clients, and their sockets, and any closing sockets
  while (!clients.empty()) {
    VNCSConnectionSpawnX* client;
    client = clients.front();
    clients.pop_front();
    delete client;
  }

  // Stop the desktop object if active, *only* after deleting all clients!
  stopDesktop();

  if (comparer)
    comparer->logStats();
  delete comparer;

  delete cursor;
}


// SocketServer methods

void VNCScreenSpawn::addSocket(network::Socket* sock, bool outgoing)
{
  throw Exception("unexpected");
}

void VNCScreenSpawn::removeSocket(network::Socket* sock) {
  throw Exception("unexpected");
}

void VNCScreenSpawn::processSocketReadEvent(network::Socket* sock)
{
  throw Exception("unexpected");
}

void VNCScreenSpawn::processSocketWriteEvent(network::Socket* sock)
{
  throw Exception("unexpected");
}

// VNCServer methods

void VNCScreenSpawn::blockUpdates()
{
  blockCounter++;

  stopFrameClock();
}

void VNCScreenSpawn::unblockUpdates()
{
  assert(blockCounter > 0);

  blockCounter--;

  // Restart the frame clock if we have updates
  if (blockCounter == 0) {
    if (!comparer->is_empty())
      startFrameClock();
  }
}

void VNCScreenSpawn::setPixelBuffer(PixelBuffer* pb_, const ScreenSet& layout)
{
  if (comparer)
    comparer->logStats();

  pb = pb_;
  delete comparer;
  comparer = 0;

  if (!pb) {
    screenLayout = ScreenSet();

    if (desktopStarted)
      throw Exception("setPixelBuffer: null PixelBuffer when desktopStarted?");

    return;
  }

  if (!layout.validate(pb->width(), pb->height()))
    throw Exception("setPixelBuffer: invalid screen layout");

  screenLayout = layout;

  // Assume the framebuffer contents wasn't saved and reset everything
  // that tracks its contents
  comparer = new ComparingUpdateTracker(pb);
  renderedCursorInvalid = true;
  add_changed(pb->getRect());

  std::list<VNCSConnectionSpawnX*>::iterator ci, ci_next;
  for (ci=clients.begin();ci!=clients.end();ci=ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->pixelBufferChange();
    // Since the new pixel buffer means an ExtendedDesktopSize needs to
    // be sent anyway, we don't need to call screenLayoutChange.
  }
}

void VNCScreenSpawn::setPixelBuffer(PixelBuffer* pb_)
{
  ScreenSet layout = screenLayout;

  // Check that the screen layout is still valid
  if (pb_ && !layout.validate(pb_->width(), pb_->height())) {
    Rect fbRect;
    ScreenSet::iterator iter, iter_next;

    fbRect.setXYWH(0, 0, pb_->width(), pb_->height());

    for (iter = layout.begin();iter != layout.end();iter = iter_next) {
      iter_next = iter; ++iter_next;
      if (iter->dimensions.enclosed_by(fbRect))
          continue;
      iter->dimensions = iter->dimensions.intersect(fbRect);
      if (iter->dimensions.is_empty()) {
        slog.info("Removing screen %d (%x) as it is completely outside the new framebuffer",
                  (int)iter->id, (unsigned)iter->id);
        layout.remove_screen(iter->id);
      }
    }
  }

  // Make sure that we have at least one screen
  if (layout.num_screens() == 0)
    layout.add_screen(Screen(0, 0, 0, pb->width(), pb->height(), 0));

  setPixelBuffer(pb_, layout);
}

void VNCScreenSpawn::setScreenLayout(const ScreenSet& layout)
{
  if (!pb)
    throw Exception("setScreenLayout: new screen layout without a PixelBuffer");
  if (!layout.validate(pb->width(), pb->height()))
    throw Exception("setScreenLayout: invalid screen layout");

  screenLayout = layout;

  std::list<VNCSConnectionSpawnX*>::iterator ci, ci_next;
  for (ci=clients.begin();ci!=clients.end();ci=ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->screenLayoutChangeOrClose(reasonServer);
  }
}

void VNCScreenSpawn::requestClipboard()
{
  if (clipboardClient == NULL)
    return;

  clipboardClient->requestClipboard();
}

void VNCScreenSpawn::announceClipboard(bool available)
{
  std::list<VNCSConnectionSpawnX*>::iterator ci, ci_next;

  if (available)
    clipboardClient = NULL;

  clipboardRequestors.clear();

  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->announceClipboard(available);
  }
}

void VNCScreenSpawn::sendClipboardData(const char* data)
{
  std::list<VNCSConnectionSpawnX*>::iterator ci, ci_next;

  if (strchr(data, '\r') != NULL)
    throw Exception("Invalid carriage return in clipboard data");

  for (ci = clipboardRequestors.begin();
       ci != clipboardRequestors.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->sendClipboardData(data);
  }

  clipboardRequestors.clear();
}

void VNCScreenSpawn::bell()
{
  std::list<VNCSConnectionSpawnX*>::iterator ci, ci_next;
  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->bellOrClose();
  }
}

void VNCScreenSpawn::setName(const char* name_)
{
  name.replaceBuf(strDup(name_));
  std::list<VNCSConnectionSpawnX*>::iterator ci, ci_next;
  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->setDesktopNameOrClose(name_);
  }
}

void VNCScreenSpawn::add_changed(const Region& region)
{
  if (comparer == NULL)
    return;

  comparer->add_changed(region);
  startFrameClock();
}

void VNCScreenSpawn::add_copied(const Region& dest, const Point& delta)
{
  if (comparer == NULL)
    return;

  comparer->add_copied(dest, delta);
  startFrameClock();
}

void VNCScreenSpawn::setCursor(int width, int height, const Point& newHotspot,
                            const rdr::U8* data)
{
  delete cursor;
  cursor = new Cursor(width, height, newHotspot, data);
  cursor->crop();

  renderedCursorInvalid = true;

  std::list<VNCSConnectionSpawnX*>::iterator ci, ci_next;
  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->renderedCursorChange();
    (*ci)->setCursorOrClose();
  }
}

void VNCScreenSpawn::setCursorPos(const Point& pos)
{
  if (!cursorPos.equals(pos)) {
    cursorPos = pos;
    renderedCursorInvalid = true;
    std::list<VNCSConnectionSpawnX*>::iterator ci;
    for (ci = clients.begin(); ci != clients.end(); ci++)
      (*ci)->renderedCursorChange();
  }
}

void VNCScreenSpawn::setLEDState(unsigned int state)
{
  std::list<VNCSConnectionSpawnX*>::iterator ci, ci_next;

  if (state == ledState)
    return;

  ledState = state;

  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->setLEDStateOrClose(state);
  }
}

// Event handlers

void VNCScreenSpawn::keyEvent(rdr::U32 keysym, rdr::U32 keycode, bool down)
{
  if (rfb::Server::maxIdleTime)
    idleTimer.start(secsToMillis(rfb::Server::maxIdleTime));

  // Remap the key if required
  if (keyRemapper) {
    rdr::U32 newkey;
    newkey = keyRemapper->remapKey(keysym);
    if (newkey != keysym) {
      slog.debug("Key remapped to 0x%x", newkey);
      keysym = newkey;
    }
  }

  SDesktop::keyEvent(keysym, keycode, down);
}

void VNCScreenSpawn::pointerEvent(VNCSConnectionSpawnX* client,
                               const Point& pos, int buttonMask)
{
  if (rfb::Server::maxIdleTime)
    idleTimer.start(secsToMillis(rfb::Server::maxIdleTime));

  // Let one client own the cursor whilst buttons are pressed in order
  // to provide a bit more sane user experience
  if ((pointerClient != NULL) && (pointerClient != client))
    return;

  if (buttonMask)
    pointerClient = client;
  else
    pointerClient = NULL;

  SDesktop::pointerEvent(pos, buttonMask);
}

void VNCScreenSpawn::handleClipboardRequest(VNCSConnectionSpawnX* client)
{
  clipboardRequestors.push_back(client);
  if (clipboardRequestors.size() == 1)
    SDesktop::handleClipboardRequest();
}

void VNCScreenSpawn::handleClipboardAnnounce(VNCSConnectionSpawnX* client,
                                          bool available)
{
  if (available)
    clipboardClient = client;
  else {
    if (client != clipboardClient)
      return;
    clipboardClient = NULL;
  }
  SDesktop::handleClipboardAnnounce(available);
}

void VNCScreenSpawn::handleClipboardData(VNCSConnectionSpawnX* client,
                                      const char* data)
{
  if (client != clipboardClient)
    return;
  SDesktop::handleClipboardData(data);
}

unsigned int VNCScreenSpawn::setDesktopSize(VNCSConnectionSpawnX* requester,
                                         int fb_width, int fb_height,
                                         const ScreenSet& layout)
{
  unsigned int result;
  std::list<VNCSConnectionSpawnX*>::iterator ci, ci_next;

  // Don't bother the desktop with an invalid configuration
  if (!layout.validate(fb_width, fb_height))
    return resultInvalid;

  // FIXME: the desktop will call back to VNCServerSpawn and an extra set
  // of ExtendedDesktopSize messages will be sent. This is okay
  // protocol-wise, but unnecessary.
  result = SDesktop::setScreenLayout(fb_width, fb_height, layout);
  if (result != resultSuccess)
    return result;

  // Sanity check
  if (screenLayout != layout)
    throw Exception("Desktop configured a different screen layout than requested");

  // Notify other clients
  for (ci=clients.begin();ci!=clients.end();ci=ci_next) {
    ci_next = ci; ci_next++;
    if ((*ci) == requester)
      continue;
    (*ci)->screenLayoutChangeOrClose(reasonOtherClient);
  }

  return resultSuccess;
}

// Other public methods


void VNCScreenSpawn::addClient(VNCSConnectionSpawnX * client)
{
  clients.push_back(client);
}

void VNCScreenSpawn::removeClient(VNCSConnectionSpawnX * client)
{
  // - Remove any references to it
  if (pointerClient == client)
    pointerClient = NULL;
  if (clipboardClient == client)
    clipboardClient = NULL;
  clipboardRequestors.remove(client);

// never stop/disconnect the desktop
//  if (authClientCount() == 0)
//    stopDesktop();

  if (comparer)
    comparer->logStats();

  clients.remove(client);

}

void VNCScreenSpawn::approveConnection(network::Socket* sock, bool accept,
                                    const char* reason)
{
  std::list<VNCSConnectionSpawnX*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock) {
      (*ci)->approveConnectionOrClose(accept, reason);
      return;
    }
  }
}

void VNCScreenSpawn::closeClients(const char* reason, network::Socket* except)
{
  std::list<VNCSConnectionSpawnX*>::iterator i, next_i;
  for (i=clients.begin(); i!=clients.end(); i=next_i) {
    next_i = i; next_i++;
    if ((*i)->getSock() != except)
      (*i)->close(reason);
  }
}

void VNCScreenSpawn::getSockets(std::list<network::Socket*>* sockets)
{
  throw Exception("unexpected");
}

SConnection* VNCScreenSpawn::getConnection(network::Socket* sock) {
  std::list<VNCSConnectionSpawnX*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock)
      return *ci;
  }
  return 0;
}

bool VNCScreenSpawn::handleTimeout(Timer* t)
{
  if (t == &frameTimer) {
    // We keep running until we go a full interval without any updates
    if (comparer->is_empty())
      return false;

    writeUpdate();

    // If this is the first iteration then we need to adjust the timeout
    if (frameTimer.getTimeoutMs() != 1000/rfb::Server::frameRate) {
      frameTimer.start(1000/rfb::Server::frameRate);
      return false;
    }

    return true;
  } else if (t == &idleTimer) {
    slog.info("MaxIdleTime reached, exiting");
    // TODO: terminate desktop grafully
//    desktop->terminate();
  } else if (t == &disconnectTimer) {
    slog.info("MaxDisconnectionTime reached, exiting");
    // TODO: terminate desktop grafully
//    desktop->terminate();
  } else if (t == &connectTimer) {
    slog.info("MaxConnectionTime reached, exiting");
    // TODO: terminate desktop grafully
//    desktop->terminate();
  }

  return false;
}

void VNCScreenSpawn::queryConnection(VNCSConnectionSpawnX* client,
                                  const char* userName)
{
  // - Authentication succeeded - clear from blacklist
  CharArray name;
  name.buf = client->getSock()->getPeerAddress();
  blHosts->clearBlackmark(name.buf);

  // - Prepare the desktop for that the client will start requiring
  // resources after this
  startDesktop();

  // - Special case to provide a more useful error message
  if (rfb::Server::neverShared &&
      !rfb::Server::disconnectClients &&
      authClientCount() > 0) {
    approveConnection(client->getSock(), false,
                      "The server is already in use");
    return;
  }

  // - Are we configured to do queries?
  if (!rfb::Server::queryConnect &&
      !client->getSock()->requiresQuery()) {
    approveConnection(client->getSock(), true, NULL);
    return;
  }

  // - Does the client have the right to bypass the query?
  if (client->accessCheck(SConnection::AccessNoQuery))
  {
    approveConnection(client->getSock(), true, NULL);
    return;
  }

// always grant access
//  desktop->queryConnection(client->getSock(), userName);
}

void VNCScreenSpawn::clientReady(VNCSConnectionSpawnX* client, bool shared)
{
  if (!shared) {
    if (rfb::Server::disconnectClients &&
        client->accessCheck(SConnection::AccessNonShared)) {
      // - Close all the other connected clients
      slog.debug("non-shared connection - closing clients");
      closeClients("Non-shared connection requested", client->getSock());
    } else {
      // - Refuse this connection if there are existing clients, in addition to
      // this one
      if (authClientCount() > 1) {
        client->close("Server is already in use");
        return;
      }
    }
  }
}

// -=- Internal methods

void VNCScreenSpawn::startDesktop()
{
  if (!desktopStarted) {
    slog.debug("starting desktop");
    XXdesktopStart(this);
    if (!pb)
      throw Exception("SDesktop::start() did not set a valid PixelBuffer");
    desktopStarted = true;
    // The tracker might have accumulated changes whilst we were
    // stopped, so flush those out
    if (!comparer->is_empty())
      writeUpdate();
  }
}

void VNCScreenSpawn::stopDesktop()
{
  if (desktopStarted) {
    slog.debug("stopping desktop");
    desktopStarted = false;
    XXdesktopStop();
    stopFrameClock();
  }
}

int VNCScreenSpawn::authClientCount() {
  int count = 0;
  std::list<VNCSConnectionSpawnX*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->authenticated())
      count++;
  }
  return count;
}

inline bool VNCScreenSpawn::needRenderedCursor()
{
  std::list<VNCSConnectionSpawnX*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++)
    if ((*ci)->needRenderedCursor()) return true;
  return false;
}

void VNCScreenSpawn::startFrameClock()
{
  if (frameTimer.isStarted())
    return;
  if (blockCounter > 0)
    return;
  if (!desktopStarted)
    return;

  // The first iteration will be just half a frame as we get a very
  // unstable update rate if we happen to be perfectly in sync with
  // the application's update rate
  frameTimer.start(1000/rfb::Server::frameRate/2);
}

void VNCScreenSpawn::stopFrameClock()
{
  frameTimer.stop();
}

int VNCScreenSpawn::msToNextUpdate()
{
  // FIXME: If the application is updating slower than frameRate then
  //        we could allow the clients more time here

  if (!frameTimer.isStarted())
    return 1000/rfb::Server::frameRate/2;
  else
    return frameTimer.getRemainingMs();
}

// writeUpdate() is called on a regular interval in order to see what
// updates are pending and propagates them to the update tracker for
// each client. It uses the ComparingUpdateTracker's compare() method
// to filter out areas of the screen which haven't actually changed. It
// also checks the state of the (server-side) rendered cursor, if
// necessary rendering it again with the correct background.

void VNCScreenSpawn::writeUpdate()
{
  UpdateInfo ui;
  Region toCheck;

  std::list<VNCSConnectionSpawnX*>::iterator ci, ci_next;

  assert(blockCounter == 0);
  assert(desktopStarted);

  comparer->getUpdateInfo(&ui, pb->getRect());
  toCheck = ui.changed.union_(ui.copied);

  if (needRenderedCursor()) {
    Rect clippedCursorRect = Rect(0, 0, cursor->width(), cursor->height())
                             .translate(cursorPos.subtract(cursor->hotspot()))
                             .intersect(pb->getRect());

    if (!toCheck.intersect(clippedCursorRect).is_empty())
      renderedCursorInvalid = true;
  }

  pb->grabRegion(toCheck);

  if (getComparerState())
    comparer->enable();
  else
    comparer->disable();

  if (comparer->compare())
    comparer->getUpdateInfo(&ui, pb->getRect());

  comparer->clear();

  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->add_copied(ui.copied, ui.copy_delta);
    (*ci)->add_changed(ui.changed);
    (*ci)->writeFramebufferUpdateOrClose();
  }
}

// checkUpdate() is called by clients to see if it is safe to read from
// the framebuffer at this time.

Region VNCScreenSpawn::getPendingRegion()
{
  UpdateInfo ui;

  // Block clients as the frame buffer cannot be safely accessed
  if (blockCounter > 0)
    return pb->getRect();

  // Block client from updating if there are pending updates
  if (comparer->is_empty())
    return Region();

  comparer->getUpdateInfo(&ui, pb->getRect());

  return ui.changed.union_(ui.copied);
}

const RenderedCursor* VNCScreenSpawn::getRenderedCursor()
{
  if (renderedCursorInvalid) {
    renderedCursor.update(pb, cursor, cursorPos);
    renderedCursorInvalid = false;
  }

  return &renderedCursor;
}

bool VNCScreenSpawn::getComparerState()
{
  if (rfb::Server::compareFB == 0)
    return false;
  if (rfb::Server::compareFB != 2)
    return true;

  std::list<VNCSConnectionSpawnX*>::iterator ci, ci_next;
  for (ci=clients.begin();ci!=clients.end();ci=ci_next) {
    ci_next = ci; ci_next++;
    if ((*ci)->getComparerState())
      return true;
  }
  return false;
}
