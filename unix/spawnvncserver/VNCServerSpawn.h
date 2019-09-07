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

// -=- VNCServerSpawnX.h

// Single-threaded VNCServer implementation

#ifndef __RFB_VNCSERVERSPAWN_H__
#define __RFB_VNCSERVERSPAWN_H__

#include <sys/time.h>

#include <rfb/SDesktop.h>
#include <rfb/VNCServer.h>
#include <rfb/Blacklist.h>
#include <rfb/Cursor.h>
#include <rfb/Timer.h>
#include <rfb/ScreenSet.h>

#include <rfb/VNCServerST.h>
#include <spawnvncserver/VNCSConnectionSpawn.h>

#include <memory>

namespace rfb {

  class VNCSConnectionSpawn;
  class VNCScreenSpawn;
  class ComparingUpdateTracker;
  class ListConnInfo;
  class PixelBuffer;
  class KeyRemapper;

  class VNCServerSpawn : public VNCServerST {
  public:
    // -=- Constructors

    //   Create a server exporting the supplied desktop.
    VNCServerSpawn(const char* name_);
    virtual ~VNCServerSpawn();


    // Methods overridden from SocketServer

    // addSocket
    //   Causes the server to allocate an RFB-protocol management
    //   structure for the socket & initialise it.
    virtual void addSocket(network::Socket* sock, bool outgoing=false) override;

    // VNCServerSpawnX-only methods

    VNCScreenSpawn * get_user_session(std::string const & username);

    void queryConnection(VNCSConnectionSpawn* client,
                                      const char* userName);

    void processXEvents();

    void getScreenSocket(std::list<VNCScreenSpawn*> & sockets);

  protected:
    std::map<std::string, std::shared_ptr<VNCScreenSpawn>> user_sessions;

  };

};

#endif

