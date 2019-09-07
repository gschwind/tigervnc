/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2009-2019 Pierre Ossman for Cendio AB
 * Copyright 2018 Peter Astrand for Cendio AB
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

#include <network/TcpSocket.h>

#include <rfb/ComparingUpdateTracker.h>
#include <rfb/Encoder.h>
#include <rfb/KeyRemapper.h>
#include <rfb/LogWriter.h>
#include <rfb/Security.h>
#include <rfb/ServerCore.h>
#include <rfb/SMsgWriter.h>
#include <rfb/screenTypes.h>
#include <rfb/fenceTypes.h>
#include <rfb/ledStates.h>
#define XK_LATIN1
#define XK_MISCELLANY
#define XK_XKB_KEYS
#include <rfb/keysymdef.h>

#include <spawnvncserver/VNCScreenSpawn.h>
#include <spawnvncserver/VNCServerSpawn.h>
#include <spawnvncserver/VNCSConnectionSpawn.h>

using namespace rfb;

static LogWriter vlog("VNCSConnST");

static Cursor emptyCursor(0, 0, Point(0, 0), NULL);

VNCSConnectionSpawn::VNCSConnectionSpawn(VNCServerSpawn* server_, network::Socket *s,
                                   bool reverse)
  : VNCSConnectionST(server_, s, reverse)
{
  setStreams(&sock->inStream(), &sock->outStream());
  peerEndpoint.buf = sock->getPeerEndpoint();

  // Configure the socket
  setSocketTimeouts();

  // Kick off the idle timer
  if (rfb::Server::idleTimeout) {
    // minimum of 15 seconds while authenticating
    if (rfb::Server::idleTimeout < 15)
      idleTimer.start(secsToMillis(15));
    else
      idleTimer.start(secsToMillis(rfb::Server::idleTimeout));
  }

}


VNCSConnectionSpawn::~VNCSConnectionSpawn()
{
//  // If we reach here then VNCServerSpawn is deleting us!
//  if (closeReason.buf)
//    vlog.info("closing %s: %s", peerEndpoint.buf, closeReason.buf);
//
//  // Release any keys the client still had pressed
//  while (!pressedKeys.empty()) {
//    rdr::U32 keysym, keycode;
//
//    keysym = pressedKeys.begin()->second;
//    keycode = pressedKeys.begin()->first;
//    pressedKeys.erase(pressedKeys.begin());
//
//    vlog.debug("Releasing key 0x%x / 0x%x on client disconnect",
//               keysym, keycode);
//    internal_server->keyEvent(keysym, keycode, false);
//  }
//
//  delete [] fenceData;
}


void VNCSConnectionSpawn::updateServer(VNCServerST * new_server)
{
  server = new_server;
  //dynamic_cast<VNCScreenSpawn*>(server)->startDesktopPublic();
}

void VNCSConnectionSpawn::queryConnection(const char* userName)
{
  dynamic_cast<VNCServerSpawn*>(server)->queryConnection(this, userName);
}

