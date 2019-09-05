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
#include <rfb/util.h>
#include <rfb/ledStates.h>

#include <rdr/types.h>

#include <spawnvncserver/XDesktop.h>
#include <spawnvncserver/VNCServerSpawn.h>
#include <spawnvncserver/VNCScreenSpawn.h>
#include <spawnvncserver/VNCSConnectionSpawn.h>

using namespace rfb;

static LogWriter slog("VNCServerSpawn");
static LogWriter connectionsLog("Connections");

//
// -=- VNCServerSpawn Implementation
//

// -=- Constructors/Destructor

VNCScreenSpawn::VNCScreenSpawn(const char* name_, SDesktop* desktop_)
  : VNCServerST(name_, desktop_)
{

}

VNCScreenSpawn::~VNCScreenSpawn()
{

}

void VNCScreenSpawn::processXEvents()
{
  dynamic_cast<XDesktop*>(desktop)->processPendingXEvent();
}

int VNCScreenSpawn::getScreenSocket()
{
  return dynamic_cast<XDesktop*>(desktop)->getFd();
}

