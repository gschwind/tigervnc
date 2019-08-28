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


