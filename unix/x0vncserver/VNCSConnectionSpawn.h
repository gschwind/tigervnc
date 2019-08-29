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

#ifndef __VNCCONNECTIONSPAWN_H__
#define __VNCCONNECTIONSPAWN_H__

#include <memory>

#include <network/TcpSocket.h>

#include <rfb/ComparingUpdateTracker.h>
#include <rfb/Encoder.h>
#include <rfb/KeyRemapper.h>
#include <rfb/LogWriter.h>
#include <rfb/Security.h>
#include <rfb/ServerCore.h>
#include <rfb/SMsgWriter.h>
#include <rfb/VNCServerST.h>

#include <rfb/VNCSConnectionST.h>

#include <x0vncserver/XDesktop.h>

namespace rfb {

struct VNCSConnectionSpawn : public VNCSConnectionST
{

  struct Server : public VNCServer {

    Server();

    void startFrameClock();
    void stopFrameClock();
    int authClientCount();

    void stopDesktop()

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


    // Methodes VNCServer

    virtual void blockUpdates() override;
    virtual void unblockUpdates() override;
    virtual void setPixelBuffer(PixelBuffer* pb, const ScreenSet& layout) override;
    virtual void setPixelBuffer(PixelBuffer* pb) override;
    virtual void setScreenLayout(const ScreenSet& layout) override;
    virtual const PixelBuffer* getPixelBuffer() const override { return pb; }

    virtual void requestClipboard() override;
    virtual void announceClipboard(bool available) override;
    virtual void sendClipboardData(const char* data) override;

    virtual void approveConnection(network::Socket* sock, bool accept,
                                   const char* reason) override;
    virtual void closeClients(const char* reason) override {closeClients(reason, 0);}
    virtual SConnection* getConnection(network::Socket* sock) override;

    virtual void add_changed(const Region &region) override;
    virtual void add_copied(const Region &dest, const Point &delta) override;
    virtual void setCursor(int width, int height, const Point& hotspot,
                           const rdr::U8* data) override;
    virtual void setCursorPos(const Point& p) override;
    virtual void setName(const char* name_) override;
    virtual void setLEDState(unsigned state) override;

    virtual void bell() override;

    Blacklist* blHosts;
    std::list<network::Socket*> closingSockets;
    Timer disconnectTimer;
    Timer connectTimer;

    // Only one client is allowed
    VNCSConnectionSpawn* pointerClient;
    std::list<VNCSConnectionSpawn*> clients;
    VNCSConnectionSpawn* clipboardClient;
    std::list<VNCSConnectionSpawn*> clipboardRequestors;

    int blockCounter;
    ComparingUpdateTracker* comparer;
    PixelBuffer* pb;
    ScreenSet screenLayout;
    bool desktopStarted;
    bool renderedCursorInvalid;
    CharArray name;
    Cursor* cursor;
    Point cursorPos;
    Timer frameTimer;

  };


  std::unique_ptr<XDesktop> desktop;
  KeyRemapper* keyRemapper;
  unsigned int ledState;

  Server server;

  VNCSConnectionSpawn(VNCServerST* server_, network::Socket* s, bool reverse);

  virtual ~VNCSConnectionSpawn();

  void keyEvent(rdr::U32 keysym, rdr::U32 keycode, bool down) override;

  void pointerEvent(const Point& pos, int buttonMask) override;

  void pixelBufferChange() override;

  unsigned getLEDState() const { return ledState; }

  // this functions comme from the server.
  void SERVERkeyEvent(rdr::U32 keysym, rdr::U32 keycode, bool down);
  void SERVERpointerEvent(VNCSConnectionST* client, const Point& pos,
      int buttonMask);

};

}

#endif
