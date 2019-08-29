/* Copyright 2019 Benoit Gschwind <gschwind@gnu-log.net>
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

#include <x0vncserver/VNCSConnectionSpawn.h>

#include <cassert>

#include <rfb/ledStates.h>

using namespace rfb;

static LogWriter vlog("VNCSConnSpawn");

// this structure ensure the shif key stay in sane state.
class VNCSConnectionSpawnShiftPresser {
public:
  VNCSConnectionSpawnShiftPresser(VNCSConnectionSpawn* server_)
    : server(server_), pressed(false) {}
  ~VNCSConnectionSpawnShiftPresser() {
    if (pressed) {
      vlog.debug("Releasing fake Shift_L");
      server->SERVERkeyEvent(XK_Shift_L, 0, false);
    }
  }
  void press() {
    vlog.debug("Pressing fake Shift_L");
    server->SERVERkeyEvent(XK_Shift_L, 0, true);
    pressed = true;
  }
  VNCSConnectionSpawn* server;
  bool pressed;
};



VNCSConnectionSpawn::Server::Server()
{
  // TODO
}

void VNCSConnectionSpawn::Server::startFrameClock()
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

void VNCSConnectionSpawn::Server::stopFrameClock()
{
  frameTimer.stop();
}

int VNCSConnectionSpawn::Server::authClientCount() {
  int count = 0;
  std::list<VNCSConnectionSpawn*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->authenticated())
      count++;
  }
  return count;
}

void VNCSConnectionSpawn::Server::stopDesktop()
{
  if (desktopStarted) {
    vlog.debug("stopping desktop");
    desktopStarted = false;
//    desktop->stop(); // TODO
    stopFrameClock();
  }
}

// SocketServer methods

void VNCSConnectionSpawn::Server::addSocket(network::Socket* sock, bool outgoing)
{
  // - Check the connection isn't black-marked
  // *** do this in getSecurity instead?
  CharArray address(sock->getPeerAddress());
  if (blHosts->isBlackmarked(address.buf)) {
    vlog.error("blacklisted: %s", address.buf);
    try {
      rdr::OutStream& os = sock->outStream();

      // Shortest possible way to tell a client it is not welcome
      os.writeBytes("RFB 003.003\n", 12);
      os.writeU32(0);
      os.writeString("Too many security failures");
      os.flush();
    } catch (rdr::Exception&) {
    }
    sock->shutdown();
    closingSockets.push_back(sock);
    return;
  }

  CharArray name;
  name.buf = sock->getPeerEndpoint();
  vlog.status("accepted: %s", name.buf);

  // Adjust the exit timers
  if (rfb::Server::maxConnectionTime && clients.empty())
    connectTimer.start(secsToMillis(rfb::Server::maxConnectionTime));
  disconnectTimer.stop();

  VNCSConnectionSpawn* client = new VNCSConnectionSpawn(this, sock, outgoing);
  clients.push_front(client);
  client->init();
}

void VNCSConnectionSpawn::Server::removeSocket(network::Socket* sock) {
  // - If the socket has resources allocated to it, delete them
  std::list<VNCSConnectionSpawn*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock) {
      // - Remove any references to it
      if (pointerClient == *ci)
        pointerClient = NULL;
      if (clipboardClient == *ci)
        clipboardClient = NULL;
      clipboardRequestors.remove(*ci);

      // Adjust the exit timers
      connectTimer.stop();
      if (rfb::Server::maxDisconnectionTime && clients.empty())
        disconnectTimer.start(secsToMillis(rfb::Server::maxDisconnectionTime));

      // - Delete the per-Socket resources
      delete *ci;

      clients.remove(*ci);

      CharArray name;
      name.buf = sock->getPeerEndpoint();
      vlog.status("closed: %s", name.buf);

      // - Check that the desktop object is still required
      if (authClientCount() == 0)
        stopDesktop();

      if (comparer)
        comparer->logStats();

      return;
    }
  }

  // - If the Socket has no resources, it may have been a closingSocket
  closingSockets.remove(sock);
}

void VNCSConnectionSpawn::Server::processSocketReadEvent(network::Socket* sock)
{
  // - Find the appropriate VNCSConnectionST and process the event
  std::list<VNCSConnectionSpawn*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock) {
      (*ci)->processMessages();
      return;
    }
  }
  throw rdr::Exception("invalid Socket in VNCServerST");
}

void VNCSConnectionSpawn::Server::processSocketWriteEvent(network::Socket* sock)
{
  // - Find the appropriate VNCSConnectionST and process the event
  std::list<VNCSConnectionSpawn*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock) {
      (*ci)->flushSocket();
      return;
    }
  }
  throw rdr::Exception("invalid Socket in VNCServerST");
}

// VNCServer methods

void VNCSConnectionSpawn::Server::blockUpdates()
{
  blockCounter++;

  stopFrameClock();
}

void VNCSConnectionSpawn::Server::unblockUpdates()
{
  assert(blockCounter > 0);

  blockCounter--;

  // Restart the frame clock if we have updates
  if (blockCounter == 0) {
    if (!comparer->is_empty())
      startFrameClock();
  }
}

void VNCSConnectionSpawn::Server::setPixelBuffer(PixelBuffer* pb_, const ScreenSet& layout)
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

  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci=clients.begin();ci!=clients.end();ci=ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->pixelBufferChange();
    // Since the new pixel buffer means an ExtendedDesktopSize needs to
    // be sent anyway, we don't need to call screenLayoutChange.
  }
}

void VNCSConnectionSpawn::Server::setPixelBuffer(PixelBuffer* pb_)
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
        vlog.info("Removing screen %d (%x) as it is completely outside the new framebuffer",
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

void VNCSConnectionSpawn::Server::setScreenLayout(const ScreenSet& layout)
{
  if (!pb)
    throw Exception("setScreenLayout: new screen layout without a PixelBuffer");
  if (!layout.validate(pb->width(), pb->height()))
    throw Exception("setScreenLayout: invalid screen layout");

  screenLayout = layout;

  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci=clients.begin();ci!=clients.end();ci=ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->screenLayoutChangeOrClose(reasonServer);
  }
}

void VNCSConnectionSpawn::Server::requestClipboard()
{
  if (clipboardClient == NULL)
    return;

  clipboardClient->requestClipboard();
}

void VNCSConnectionSpawn::Server::announceClipboard(bool available)
{
  std::list<VNCSConnectionST*>::iterator ci, ci_next;

  if (available)
    clipboardClient = NULL;

  clipboardRequestors.clear();

  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->announceClipboard(available);
  }
}

void VNCSConnectionSpawn::Server::sendClipboardData(const char* data)
{
  std::list<VNCSConnectionST*>::iterator ci, ci_next;

  if (strchr(data, '\r') != NULL)
    throw Exception("Invalid carriage return in clipboard data");

  for (ci = clipboardRequestors.begin();
       ci != clipboardRequestors.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->sendClipboardData(data);
  }

  clipboardRequestors.clear();
}

void VNCSConnectionSpawn::Server::bell()
{
  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->bellOrClose();
  }
}

void VNCSConnectionSpawn::Server::setName(const char* name_)
{
  name.replaceBuf(strDup(name_));
  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->setDesktopNameOrClose(name_);
  }
}

void VNCSConnectionSpawn::Server::add_changed(const Region& region)
{
  if (comparer == NULL)
    return;

  comparer->add_changed(region);
  startFrameClock();
}

void VNCSConnectionSpawn::Server::add_copied(const Region& dest, const Point& delta)
{
  if (comparer == NULL)
    return;

  comparer->add_copied(dest, delta);
  startFrameClock();
}

void VNCSConnectionSpawn::Server::setCursor(int width, int height, const Point& newHotspot,
                            const rdr::U8* data)
{
  delete cursor;
  cursor = new Cursor(width, height, newHotspot, data);
  cursor->crop();

  renderedCursorInvalid = true;

  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->renderedCursorChange();
    (*ci)->setCursorOrClose();
  }
}

void VNCSConnectionSpawn::Server::setCursorPos(const Point& pos)
{
  if (!cursorPos.equals(pos)) {
    cursorPos = pos;
    renderedCursorInvalid = true;
    std::list<VNCSConnectionST*>::iterator ci;
    for (ci = clients.begin(); ci != clients.end(); ci++)
      (*ci)->renderedCursorChange();
  }
}

void VNCSConnectionSpawn::Server::setLEDState(unsigned int state)
{
  std::list<VNCSConnectionST*>::iterator ci, ci_next;

  if (state == ledState)
    return;

  ledState = state;

  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->setLEDStateOrClose(state);
  }
}



VNCSConnectionSpawn::VNCSConnectionSpawn(VNCServerST* server_, network::Socket* s, bool reverse) :
  VNCSConnectionST(server_, s, reverse),
  desktop{nullptr},
  keyRemapper{&KeyRemapper::defInstance},
  ledState{ledUnknown}
{

}

// keyEvent() - record in the pressedKeys which keys were pressed.  Allow
// multiple down events (for autorepeat), but only allow a single up event.
void VNCSConnectionSpawn::keyEvent(rdr::U32 keysym, rdr::U32 keycode, bool down) {
  rdr::U32 lookup;

  if (rfb::Server::idleTimeout)
    idleTimer.start(secsToMillis(rfb::Server::idleTimeout));
  if (!accessCheck(AccessKeyEvents)) return;
  if (!rfb::Server::acceptKeyEvents) return;

  if (down)
    vlog.debug("Key pressed: 0x%x / 0x%x", keysym, keycode);
  else
    vlog.debug("Key released: 0x%x / 0x%x", keysym, keycode);

  // Avoid lock keys if we don't know the server state
  if ((ledState == ledUnknown) &&
      ((keysym == XK_Caps_Lock) ||
       (keysym == XK_Num_Lock) ||
       (keysym == XK_Scroll_Lock))) {
    vlog.debug("Ignoring lock key (e.g. caps lock)");
    return;
  }

  // Lock key heuristics
  // (only for clients that do not support the LED state extension)
  if (!client.supportsLEDState()) {
    // Always ignore ScrollLock as we don't have a heuristic
    // for that
    if (keysym == XK_Scroll_Lock) {
      vlog.debug("Ignoring lock key (e.g. caps lock)");
      return;
    }

    if (down && (ledState != ledUnknown)) {
      // CapsLock synchronisation heuristic
      // (this assumes standard interaction between CapsLock the Shift
      // keys and normal characters)
      if (((keysym >= XK_A) && (keysym <= XK_Z)) ||
          ((keysym >= XK_a) && (keysym <= XK_z))) {
        bool uppercase, shift, lock;

        uppercase = (keysym >= XK_A) && (keysym <= XK_Z);
        shift = isShiftPressed();
        lock = ledState & ledCapsLock;

        if (lock == (uppercase == shift)) {
          vlog.debug("Inserting fake CapsLock to get in sync with client");
          SERVERkeyEvent(XK_Caps_Lock, 0, true);
          SERVERkeyEvent(XK_Caps_Lock, 0, false);
        }
      }

      // NumLock synchronisation heuristic
      // (this is more cautious because of the differences between Unix,
      // Windows and macOS)
      if (((keysym >= XK_KP_Home) && (keysym <= XK_KP_Delete)) ||
          ((keysym >= XK_KP_0) && (keysym <= XK_KP_9)) ||
          (keysym == XK_KP_Separator) || (keysym == XK_KP_Decimal)) {
        bool number, shift, lock;

        number = ((keysym >= XK_KP_0) && (keysym <= XK_KP_9)) ||
                  (keysym == XK_KP_Separator) || (keysym == XK_KP_Decimal);
        shift = isShiftPressed();
        lock = getLEDState() & ledNumLock;

        if (shift) {
          // We don't know the appropriate NumLock state for when Shift
          // is pressed as it could be one of:
          //
          // a) A Unix client where Shift negates NumLock
          //
          // b) A Windows client where Shift only cancels NumLock
          //
          // c) A macOS client where Shift doesn't have any effect
          //
        } else if (lock == (number == shift)) {
          vlog.debug("Inserting fake NumLock to get in sync with client");
          SERVERkeyEvent(XK_Num_Lock, 0, true);
          SERVERkeyEvent(XK_Num_Lock, 0, false);
        }
      }
    }
  }

  // Turn ISO_Left_Tab into shifted Tab.

  // this structure will release shift if needed on destroy, i.e. on function
  // return, even in case of throw.
  VNCSConnectionSpawnShiftPresser shiftPresser(this);
  if (keysym == XK_ISO_Left_Tab) {
    if (!isShiftPressed())
      shiftPresser.press();
    keysym = XK_Tab;
  }

  // We need to be able to track keys, so generate a fake index when we
  // aren't given a keycode
  if (keycode == 0)
    lookup = 0x80000000 | keysym;
  else
    lookup = keycode;

  // We force the same keysym for an already down key for the
  // sake of sanity
  if (pressedKeys.find(lookup) != pressedKeys.end())
    keysym = pressedKeys[lookup];

  if (down) {
    pressedKeys[lookup] = keysym;
  } else {
    if (!pressedKeys.erase(lookup))
      return;
  }

  SERVERkeyEvent(keysym, keycode, down);
}

void VNCSConnectionSpawn::pointerEvent(const Point& pos, int buttonMask)
{
  if (rfb::Server::idleTimeout)
    idleTimer.start(secsToMillis(rfb::Server::idleTimeout));
  pointerEventTime = time(0);
  if (!accessCheck(AccessPtrEvents)) return;
  if (!rfb::Server::acceptPointerEvents) return;
  pointerEventPos = pos;
  SERVERpointerEvent(this, pointerEventPos, buttonMask);
}

// Event handlers

void VNCSConnectionSpawn::SERVERkeyEvent(rdr::U32 keysym, rdr::U32 keycode, bool down)
{
  // Remap the key if required
  if (keyRemapper) {
    rdr::U32 newkey;
    newkey = keyRemapper->remapKey(keysym);
    if (newkey != keysym) {
      vlog.debug("Key remapped to 0x%x", newkey);
      keysym = newkey;
    }
  }

  desktop->keyEvent(keysym, keycode, down);
}

void VNCSConnectionSpawn::SERVERpointerEvent(VNCSConnectionST* client,
                               const Point& pos, int buttonMask)
{
  if (rfb::Server::maxIdleTime)
    idleTimer.start(secsToMillis(rfb::Server::maxIdleTime));

  // Let one client own the cursor whilst buttons are pressed in order
  // to provide a bit more sane user experience
  // TODO
  //if ((pointerClient != NULL) && (pointerClient != client))
  //  return;
//
//  if (buttonMask)
//    pointerClient = client;
//  else
//    pointerClient = NULL;

  desktop->pointerEvent(pos, buttonMask);
}

VNCSConnectionSpawn::~VNCSConnectionSpawn()
{
  // If we reach here then VNCServerST is deleting us!
  if (closeReason.buf)
    vlog.info("closing %s: %s", peerEndpoint.buf, closeReason.buf);

  // Release any keys the client still had pressed
  while (!pressedKeys.empty()) {
    rdr::U32 keysym, keycode;

    keysym = pressedKeys.begin()->second;
    keycode = pressedKeys.begin()->first;
    pressedKeys.erase(pressedKeys.begin());

    vlog.debug("Releasing key 0x%x / 0x%x on client disconnect",
               keysym, keycode);
    SERVERkeyEvent(keysym, keycode, false);
  }

  delete [] fenceData;
}

void VNCSConnectionSpawn::pixelBufferChange()
{
  try {
    if (!authenticated()) return;
    if (client.width() && client.height() &&
        (server->getPixelBuffer()->width() != client.width() ||
         server->getPixelBuffer()->height() != client.height()))
    {
      // We need to clip the next update to the new size, but also add any
      // extra bits if it's bigger.  If we wanted to do this exactly, something
      // like the code below would do it, but at the moment we just update the
      // entire new size.  However, we do need to clip the damagedCursorRegion
      // because that might be added to updates in writeFramebufferUpdate().

      //updates.intersect(server->pb->getRect());
      //
      //if (server->pb->width() > client.width())
      //  updates.add_changed(Rect(client.width(), 0, server->pb->width(),
      //                           server->pb->height()));
      //if (server->pb->height() > client.height())
      //  updates.add_changed(Rect(0, client.height(), client.width(),
      //                           server->pb->height()));

      damagedCursorRegion.assign_intersect(server->getPixelBuffer()->getRect());

      client.setDimensions(server->getPixelBuffer()->width(),
                           server->getPixelBuffer()->height(),
                           server->getScreenLayout());
      if (state() == RFBSTATE_NORMAL) {
        if (!client.supportsDesktopSize()) {
          close("Client does not support desktop resize");
          return;
        }
        writer()->writeDesktopSize(reasonServer);
      }

      // Drop any lossy tracking that is now outside the framebuffer
      encodeManager.pruneLosslessRefresh(Region(server->getPixelBuffer()->getRect()));
    }
    // Just update the whole screen at the moment because we're too lazy to
    // work out what's actually changed.
    updates.clear();
    updates.add_changed(server->getPixelBuffer()->getRect());
    writeFramebufferUpdate();
  } catch(rdr::Exception &e) {
    close(e.str());
  }
}


