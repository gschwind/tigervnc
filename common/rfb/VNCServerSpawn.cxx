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
// VNCServerSpawnX must explicitly know about all the sockets (so that it can block
// on them appropriately).  However, VNCServerSpawnX may want to close clients for
// a number of reasons, and from a variety of entry points.  The simplest is
// when processSocketEvent() is called for a client, and the remote end has
// closed its socket.  A more complex reason is when processSocketEvent() is
// called for a client which has just sent a ClientInit with the shared flag
// set to false - in this case we want to close all other clients.  Yet another
// reason for disconnecting clients is when the desktop size has changed as a
// result of a call to setPixelBuffer().
//
// The responsibility for creating and deleting sockets is entirely with the
// calling code.  When VNCServerSpawnX wants to close a connection to a client it
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
#include <rfb/VNCServerSpawn.h>
#include <rfb/VNCSConnectionSpawn.h>
#include <rfb/util.h>
#include <rfb/ledStates.h>

#include <rdr/types.h>

using namespace rfb;

static LogWriter slog("VNCServerSpawnX");
static LogWriter connectionsLog("Connections");

//
// -=- VNCServerSpawnX Implementation
//

// -=- Constructors/Destructor

VNCServerSpawn::VNCServerSpawn(const char* name_)
  : blHosts(&blacklist), name(strDup(name_)),
    idleTimer(this), disconnectTimer(this), connectTimer(this)
{
  slog.debug("creating single-threaded server %s", name.buf);

  // FIXME: Do we really want to kick off these right away?
  if (rfb::Server::maxIdleTime)
    idleTimer.start(secsToMillis(rfb::Server::maxIdleTime));
  if (rfb::Server::maxDisconnectionTime)
    disconnectTimer.start(secsToMillis(rfb::Server::maxDisconnectionTime));
}

VNCServerSpawn::~VNCServerSpawn()
{
  slog.debug("shutting down server %s", name.buf);

  // Close any active clients, with appropriate logging & cleanup
  for (auto &x: user_sessions) {
    x.second.reset();
  }

  // Delete all the clients, and their sockets, and any closing sockets
  while (!clients.empty()) {
    VNCSConnectionSpawn* client;
    client = clients.front();
    clients.pop_front();
    delete client;
  }

}


// SocketServer methods

void VNCServerSpawn::addSocket(network::Socket* sock, bool outgoing)
{
  // - Check the connection isn't black-marked
  // *** do this in getSecurity instead?
  CharArray address(sock->getPeerAddress());
  if (blHosts->isBlackmarked(address.buf)) {
    connectionsLog.error("blacklisted: %s", address.buf);
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
  connectionsLog.status("accepted: %s", name.buf);

  // Adjust the exit timers
  if (rfb::Server::maxConnectionTime && clients.empty())
    connectTimer.start(secsToMillis(rfb::Server::maxConnectionTime));
  disconnectTimer.stop();

  VNCSConnectionSpawn* client = new VNCSConnectionSpawn(this, sock, outgoing);
  clients.push_front(client);
  client->init();
}

void VNCServerSpawn::removeSocket(network::Socket* sock) {
  // - If the socket has resources allocated to it, delete them
  std::list<VNCSConnectionSpawn*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock) {

      // remove the client from the internal server.
      (*ci)->unregister();

      // Adjust the exit timers
      connectTimer.stop();
      if (rfb::Server::maxDisconnectionTime && clients.empty())
        disconnectTimer.start(secsToMillis(rfb::Server::maxDisconnectionTime));

      // - Delete the per-Socket resources
      delete *ci;
      clients.remove(*ci);

      CharArray name;
      name.buf = sock->getPeerEndpoint();
      connectionsLog.status("closed: %s", name.buf);

      return;
    }
  }

  // - If the Socket has no resources, it may have been a closingSocket
  closingSockets.remove(sock);
}

void VNCServerSpawn::processSocketReadEvent(network::Socket* sock)
{
  // - Find the appropriate VNCSConnectionSpawnX and process the event
  std::list<VNCSConnectionSpawn*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock) {
      (*ci)->processMessages();
      return;
    }
  }
  throw rdr::Exception("invalid Socket in VNCServerSpawnX");
}

void VNCServerSpawn::processSocketWriteEvent(network::Socket* sock)
{
  // - Find the appropriate VNCSConnectionSpawnX and process the event
  std::list<VNCSConnectionSpawn*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock) {
      (*ci)->flushSocket();
      return;
    }
  }
  throw rdr::Exception("invalid Socket in VNCServerSpawnX");
}

VNCScreenSpawn * VNCServerSpawn::get_user_session(std::string const & userName)
{
  auto x = user_sessions.find(userName);
  if (x != user_sessions.end()) {
    return x->second.get();
  } else {
    auto session = std::make_shared<VNCServerSpawn>("DummyServerName", create_sdesktop(userName));
    user_sessions[userName] = session;
    return session.get();
  }
}




// Other public methods

void VNCServerSpawn::closeClients(const char* reason, network::Socket* except)
{
  std::list<VNCSConnectionSpawn*>::iterator i, next_i;
  for (i=clients.begin(); i!=clients.end(); i=next_i) {
    next_i = i; next_i++;
    if ((*i)->getSock() != except)
      (*i)->close(reason);
  }
}

void VNCServerSpawn::getSockets(std::list<network::Socket*>* sockets)
{
  sockets->clear();
  std::list<VNCSConnectionSpawn*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    sockets->push_back((*ci)->getSock());
  }
  std::list<network::Socket*>::iterator si;
  for (si = closingSockets.begin(); si != closingSockets.end(); si++) {
    sockets->push_back(*si);
  }
}

// -=- Internal methods
bool VNCServerSpawn::handleTimeout(Timer* t)
{
  //TODO
  return false;
}

int VNCServerSpawn::authClientCount() {
  int count = 0;
  std::list<VNCSConnectionSpawn*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->authenticated())
      count++;
  }
  return count;
}
