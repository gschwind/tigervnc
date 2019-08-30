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

#ifndef __RFB_VNCSERVERSPAWNX_H__
#define __RFB_VNCSERVERSPAWNX_H__

#include <sys/time.h>

#include <rfb/SDesktop.h>
#include <rfb/VNCServer.h>
#include <rfb/Blacklist.h>
#include <rfb/Cursor.h>
#include <rfb/Timer.h>
#include <rfb/ScreenSet.h>

#include <memory>

namespace rfb {

  class VNCSConnectionSpawnX;
  class VNCServerSpawn;
  class ComparingUpdateTracker;
  class ListConnInfo;
  class PixelBuffer;
  class KeyRemapper;

  class VNCServerSpawnXBase : public network::SocketServer,
                          public Timer::Callback {
  public:
    // -=- Constructors

    //   Create a server exporting the supplied desktop.
    VNCServerSpawnXBase(const char* name_);
    virtual ~VNCServerSpawnXBase();


    // Methods overridden from SocketServer

    // addSocket
    //   Causes the server to allocate an RFB-protocol management
    //   structure for the socket & initialise it.
    virtual void addSocket(network::Socket* sock, bool outgoing=false);

    // removeSocket
    //   Clean up any resources associated with the Socket
    virtual void removeSocket(network::Socket* sock);

    // getSockets() gets a list of sockets.  This can be used to generate an
    // fd_set for calling select().
    virtual void getSockets(std::list<network::Socket*>* sockets);

    // processSocketReadEvent
    //   Read more RFB data from the Socket.  If an error occurs during
    //   processing then shutdown() is called on the Socket, causing
    //   removeSocket() to be called by the caller at a later time.
    virtual void processSocketReadEvent(network::Socket* sock);

    // processSocketWriteEvent
    //   Flush pending data from the Socket on to the network.
    virtual void processSocketWriteEvent(network::Socket* sock);


    // VNCServerSpawnX-only methods

    VNCServerSpawn * get_user_session(std::string const & username);

    // closeClients() closes all RFB sessions, except the specified one (if
    // any), and logs the specified reason for closure.
    void closeClients(const char* reason, network::Socket* sock);

    // Part of the framebuffer that has been modified but is not yet
    // ready to be sent to clients
    Region getPendingRegion();

    // getRenderedCursor() returns an up to date version of the server
    // side rendered cursor buffer
    const RenderedCursor* getRenderedCursor();

  protected:

    virtual SDesktop * create_sdesktop(std::string const & userName) = 0;

    // Timer callbacks
    virtual bool handleTimeout(Timer* t);

    // - Internal methods

    // - Check how many of the clients are authenticated.
    int authClientCount();

  protected:
    std::map<std::string, std::shared_ptr<VNCServerSpawn>> user_sessions;

    Blacklist blacklist;
    Blacklist* blHosts;

    CharArray name;

    std::list<VNCSConnectionSpawnX*> clients;
    std::list<network::Socket*> closingSockets;

    Timer idleTimer;
    Timer disconnectTimer;
    Timer connectTimer;

  };

  template<typename SDesktopType>
  class VNCServerSpawnX : public VNCServerSpawnXBase
  {
    virtual SDesktop * create_sdesktop() override {
      return new SDesktopType();
    }
  };

};

#endif

