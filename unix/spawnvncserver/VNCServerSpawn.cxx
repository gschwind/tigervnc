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
#include <rfb/util.h>
#include <rfb/ledStates.h>

#include <rdr/types.h>

#include <spawnvncserver/VNCScreenSpawn.h>
#include <spawnvncserver/VNCServerSpawn.h>
#include <spawnvncserver/VNCSConnectionSpawn.h>
#include <spawnvncserver/XDesktop.h>

using namespace rfb;

static LogWriter slog("VNCServerSpawnX");
static LogWriter connectionsLog("Connections");

//
// -=- VNCServerSpawnX Implementation
//

// -=- Constructors/Destructor

VNCServerSpawn::VNCServerSpawn(const char* name_)
  : VNCServerST(name_, nullptr)
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

  // at begining client connection tal with us.
  VNCSConnectionSpawn* client = new VNCSConnectionSpawn(this, sock, outgoing);
  clients.push_front(client);
  client->init();
}

VNCScreenSpawn * VNCServerSpawn::get_user_session(std::string const & userName)
{
  auto x = user_sessions.find(userName);
  if (x != user_sessions.end()) {
    return x->second.get();
  } else {
    auto desktop = new XDesktop(user_sessions.size()+10, userName);
    auto session = std::make_shared<VNCScreenSpawn>("DummyServerName", desktop);
    user_sessions[userName] = session;
    return session.get();
  }
}

void VNCServerSpawn::queryConnection(VNCSConnectionSpawn* client,
                                  const char* userName)
{
  // - Authentication succeeded - clear from blacklist
  CharArray name;
  name.buf = client->getSock()->getPeerAddress();
  blHosts->clearBlackmark(name.buf);

  // FIXME: start a session event if the client doesn't get approved.
  auto server = get_user_session(userName);
  client->updateServer(server);
  server->addClient(client);
  server->startDesktopPublic();

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

}


void VNCServerSpawn::processXEvents()
{
  for(auto &x: user_sessions) {
    x.second->processXEvents();
  }
}

void VNCServerSpawn::getScreenSocket(std::list<VNCScreenSpawn*> & sockets)
{
  for(auto &x: user_sessions) {
    sockets.push_back(x.second.get());
  }
}

