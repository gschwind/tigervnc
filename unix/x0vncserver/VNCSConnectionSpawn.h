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

namespace rfb {

class VNCSConnectionSpawn : public VNCSConnectionST
{

public:
  VNCSConnectionSpawn(VNCServerST* server_, network::Socket* s, bool reverse);

};

}

#endif
