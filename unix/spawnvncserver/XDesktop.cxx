/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright (C) 2004-2008 Constantin Kaplinsky.  All Rights Reserved.
 * Copyright 2017 Peter Astrand <astrand@cendio.se> for Cendio AB
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

#include <assert.h>
#include <signal.h>
#include <unistd.h>

#include <rfb/LogWriter.h>

#include <spawnvncserver/XDesktop.h>

// C++ HACK until xkb do not use explicit anymore.
#define explicit c_explicit
#include <xcb/xkb.h>
#undef explicit

#include <X11/XKBlib.h>

#ifdef HAVE_XTEST
#include <X11/extensions/XTest.h>
#endif
#ifdef HAVE_XDAMAGE
#include <X11/extensions/Xdamage.h>
#include <xcb/damage.h>
#endif
#ifdef HAVE_XFIXES
#include <X11/extensions/Xfixes.h>
#include <xcb/xfixes.h>
#endif
#ifdef HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#include <RandrGlue.h>
#include <xcb/randr.h>
extern "C" {
void vncSetGlueContext(Display *dpy, void *res);
}
#endif
#include <spawnvncserver/Geometry.h>
#include <spawnvncserver/XPixelBuffer.h>

#include <common/unixcommon.h>

#include <xcb/xtest.h>

using namespace rfb;

extern const unsigned short code_map_qnum_to_xorgevdev[];
extern const unsigned int code_map_qnum_to_xorgevdev_len;

extern const unsigned short code_map_qnum_to_xorgkbd[];
extern const unsigned int code_map_qnum_to_xorgkbd_len;

BoolParameter useShm("UseSHM", "Use MIT-SHM extension if available", true);
BoolParameter rawKeyboard("RawKeyboard",
                          "Send keyboard events straight through and "
                          "avoid mapping them to the current keyboard "
                          "layout", false);
IntParameter queryConnectTimeout("QueryConnectTimeout",
                                 "Number of seconds to show the Accept Connection dialog before "
                                 "rejecting the connection",
                                 10);

static rfb::LogWriter vlog("XDesktop");

// order is important as it must match RFB extension
static const char * ledNames[XDESKTOP_N_LEDS] = {
  "Scroll Lock", "Num Lock", "Caps Lock"
};

xcb_screen_t * _screen_of_display(xcb_connection_t *c, int screen)
{
  xcb_screen_iterator_t iter;

  iter = xcb_setup_roots_iterator(xcb_get_setup(c));
  for (; iter.rem; --screen, xcb_screen_next(&iter))
    if (screen == 0)
      return iter.data;

  return NULL;
}

bool XDesktop::queryExtension(char const * name, int * opcode, int * event, int * error) const
{
  xcb_generic_error_t * err;
  xcb_query_extension_cookie_t ck = xcb_query_extension(xcb, strlen(name), name);
  xcb_query_extension_reply_t * r = xcb_query_extension_reply(xcb, ck, &err);
  if (err != nullptr or r == nullptr) {
    return false;
  } else {
    if (opcode)
      *opcode = r->major_opcode;
    if (event)
      *event = r->first_event;
    if (error)
      *error = r->first_error;
    free(r);
    return true;
  }
}

XDesktop::XDesktop(char const * displayName)
  : xcb(0), geometry(0, 0), pb(0), server(0),
    queryConnectDialog(0), queryConnectSock(0),
    oldButtonMask(0), haveXtest(false),
    maxButtons(0), running(false), ledMasks(), ledState(0),
    codeMap(0), codeMapLen(0)
{

  for(int i = 0; i < 10; ++i) { // 10 atempt.
    sleep(1);
    xcb = xcb_connect(displayName, &default_screen);
    if (not xcb_connection_has_error(xcb))
      break;
    xcb_disconnect(xcb);
    xcb = nullptr;
  }


  if (!xcb) {
    // FIXME: Why not vlog.error(...)?
    fprintf(stderr,"%s: unable to open display \"%s\"\r\n",
            "TODO", displayName);
    throw Exception();
  }

  default_root = _screen_of_display(xcb, default_screen)->root;
  vlog.debug("Root win id = 0x%x", default_root);

  update_default_visual();

  {
    auto c = xcb_get_geometry(xcb, default_root);
    auto r = xcb_get_geometry_reply(xcb, c, nullptr);
    if (!r)
      throw Exception("Error while getting geometry");
    geometry = Geometry(r->width, r->height);
    free(r);
  }

  if (not queryExtension("XKEYBOARD", nullptr, &xkbEventBase, nullptr)) {
    vlog.error("XKEYBOARD extension not present");
    throw Exception();
  } else {
    xcb_generic_error_t * e;
    auto c = xcb_xkb_use_extension(xcb, XCB_XKB_MAJOR_VERSION, XCB_XKB_MINOR_VERSION);
    auto r = xcb_xkb_use_extension_reply(xcb, c, &e);
    if (r == nullptr or e != nullptr) {
      vlog.error("XKEYBOARD extension not present");
      throw Exception();
    }
    free(r);
  }

  uint16_t all_map_parts = XCB_XKB_MAP_PART_KEY_TYPES |
                        XCB_XKB_MAP_PART_KEY_SYMS |
                        XCB_XKB_MAP_PART_MODIFIER_MAP |
                        XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS |
                        XCB_XKB_MAP_PART_KEY_ACTIONS |
                        XCB_XKB_MAP_PART_KEY_BEHAVIORS |
                        XCB_XKB_MAP_PART_VIRTUAL_MODS |
                        XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP;

  xcb_xkb_select_events(xcb, XCB_XKB_ID_USE_CORE_KBD,
      XCB_XKB_EVENT_TYPE_INDICATOR_STATE_NOTIFY,
      0, // do not clear
      XCB_XKB_EVENT_TYPE_INDICATOR_STATE_NOTIFY, // select all
      all_map_parts, all_map_parts, // for all part
      nullptr);

  // figure out bit masks for the indicators we are interested in
  for (int i = 0; i < XDESKTOP_N_LEDS; i++) {
    xcb_generic_error_t * e;
    auto c0 = xcb_intern_atom(xcb, True, strlen(ledNames[i]), ledNames[i]);
    auto a = xcb_intern_atom_reply(xcb, c0, &e);
    if (not a)
      continue;
    auto c1 = xcb_xkb_get_named_indicator(xcb, XCB_XKB_ID_USE_CORE_KBD,
        XCB_XKB_LED_CLASS_DFLT_XI_CLASS, XCB_XKB_ID_DFLT_XI_ID, a->atom);
    auto r = xcb_xkb_get_named_indicator_reply(xcb, c1, &e);
    free(a);
    if (not r)
      continue;

    ledMasks[i] = 1u << r->ndx;
    vlog.debug("Mask for '%s' is 0x%x", ledNames[i], ledMasks[i]);
    if (r->on)
      ledState |= 1u << i;
    free(r);
  }

  // X11 unfortunately uses keyboard driver specific keycodes and provides no
  // direct way to query this, so guess based on the keyboard mapping
  xcb_generic_error_t * e;
//  auto c = xcb_xkb_get_kbd_by_name(xcb, XCB_XKB_ID_USE_CORE_KBD, XCB_XKB_GBN_DETAIL_KEY_NAMES, 0, False);
//  auto r = xcb_xkb_get_kbd_by_name_reply(xcb, c, &e);

  auto c = xcb_xkb_get_names(xcb, XCB_XKB_ID_USE_CORE_KBD, XCB_XKB_NAME_DETAIL_KEYCODES);
  auto r = xcb_xkb_get_names_reply(xcb, c, &e);
  if (r) {
    auto names_value_list = reinterpret_cast<xcb_xkb_get_names_value_list_t*>(xcb_xkb_get_names_value_list(r));

    auto c = xcb_get_atom_name(xcb, names_value_list->keycodesName);
    auto r = xcb_get_atom_name_reply(xcb, c, &e);
    char *keycodes = xcb_get_atom_name_name(r);

    vlog.info("keycodeName = %s", xcb_get_atom_name_name(r));

    if (keycodes) {
      if (strncmp("evdev", keycodes, strlen("evdev")) == 0) {
        codeMap = code_map_qnum_to_xorgevdev;
        codeMapLen = code_map_qnum_to_xorgevdev_len;
        vlog.info("Using evdev codemap\n");
      } else if (strncmp("xfree86", keycodes, strlen("xfree86")) == 0) {
        codeMap = code_map_qnum_to_xorgkbd;
        codeMapLen = code_map_qnum_to_xorgkbd_len;
        vlog.info("Using xorgkbd codemap\n");
      } else {
        vlog.info("Unknown keycode '%s', no codemap\n", keycodes);
      }
    } else {
      vlog.debug("Unable to get keycode map\n");
    }

    free(r);

  }

  free(r);

#ifdef HAVE_XTEST
  if (queryExtension("XTEST")) {
    xcb_generic_error_t * e;
    auto c = xcb_test_get_version(xcb, XCB_TEST_MAJOR_VERSION, XCB_TEST_MINOR_VERSION);
    auto r = xcb_test_get_version_reply(xcb, c, &e);
    if (r != nullptr and e == nullptr) {
      vlog.info("XTest extension present - version %d.%d",r->major_version,r->minor_version);
      xcb_test_grab_control(xcb, True);
      haveXtest = true;
    }
    free(r);
  }

  if (not haveXtest) {
#endif
    vlog.info("XTest extension not present");
    vlog.info("Unable to inject events or display while server is grabbed");
#ifdef HAVE_XTEST
  }
#endif

  damage = XCB_NONE;

#ifdef HAVE_XDAMAGE
  if (queryExtension("DAMAGE", nullptr, &xdamageEventBase, nullptr)) {
    vlog.info("DAMAGE extension found %d", xdamageEventBase);
    xcb_generic_error_t * e;
    auto c = xcb_damage_query_version(xcb, XCB_DAMAGE_MAJOR_VERSION, XCB_DAMAGE_MINOR_VERSION);
    auto r = xcb_damage_query_version_reply(xcb, c, &e);
    if (r != nullptr and e == nullptr) {
      vlog.info("DAMAGE extension present - version %d.%d",r->major_version,r->minor_version);
    }
    free(r);
  } else {
#endif
    vlog.info("DAMAGE extension not present");
    vlog.info("Will have to poll screen for changes");
    throw Exception("DAMAGE extension is mandatory");
#ifdef HAVE_XDAMAGE
  }
#endif

#ifdef HAVE_XFIXES
  if (queryExtension(XFIXES_NAME, nullptr, &xfixesEventBase, nullptr)) {
    {
      xcb_generic_error_t * e;
      auto c = xcb_xfixes_query_version(xcb, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);
      auto r = xcb_xfixes_query_version_reply(xcb, c, &e);
      if (r != nullptr and e == nullptr) {
        vlog.info("Xfixes extension present - version %d.%d",r->major_version,r->minor_version);
        xcb_xfixes_select_cursor_input(xcb, default_root, XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR);
      } else {
        vlog.error("Xfixes not found");
        throw Exception();
      }
      free(r);
    }

  } else {
#endif
    vlog.info("XFIXES extension not present");
    vlog.info("Will not be able to display cursors");
#ifdef HAVE_XFIXES
  }
#endif

#ifdef HAVE_XRANDR

  randrSyncSerial = 0;
  if (queryExtension("RANDR", nullptr, &xrandrEventBase, nullptr)) {
    xcb_generic_error_t * e;
    auto c = xcb_randr_query_version(xcb, XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION);
    auto r = xcb_randr_query_version_reply(xcb, c, &e);
    if (r != nullptr and e == nullptr) {
      vlog.info("RANDR extension present - version %d.%d",r->major_version,r->minor_version);
    }
    free(r);
    xcb_randr_select_input(xcb, default_root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE|XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE);

    /* Override TXWindow::init input mask */
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE
        | XCB_EVENT_MASK_EXPOSURE
        | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(xcb, default_root, XCB_CW_EVENT_MASK, &mask);

  } else {
#endif
    vlog.info("RANDR extension not present");
    vlog.info("Will not be able to handle session resize");
#ifdef HAVE_XRANDR
  }
#endif

}

XDesktop::~XDesktop() {
  if (running)
    stop();
}


void XDesktop::poll() {
  if (running) {
    int x, y;
    xcb_generic_error_t * e;
    auto c = xcb_query_pointer(xcb, default_root);
    auto r = xcb_query_pointer_reply(xcb, c, &e);
    x = r->root_x - geometry.offsetLeft();
    y = r->root_y - geometry.offsetTop();
    free(r);
    server->setCursorPos(rfb::Point(x, y));
  }
}

void XDesktop::processPendingXEvent()
{
  xcb_generic_event_t * ev;
  while((ev = xcb_poll_for_event(xcb))) {
    handleGlobalEvent(ev);
  }
}

void XDesktop::update_default_visual()
{

  /* you init the connection and screen_nbr */
  screen = _screen_of_display(xcb, default_screen);

  printf("found screen %p\n", screen);
  if (screen != nullptr) {
    xcb_depth_iterator_t depth_iter;
    depth_iter = xcb_screen_allowed_depths_iterator(screen);
    for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
      xcb_visualtype_iterator_t visual_iter;

      visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
      for (; visual_iter.rem; xcb_visualtype_next(&visual_iter)) {

        xcb_visual_data[visual_iter.data->visual_id] = visual_iter.data;
        xcb_visual_depth[visual_iter.data->visual_id] = depth_iter.data->depth;

        if(visual_iter.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR
            and depth_iter.data->depth == 32) {
          xcb_default_visual_type = visual_iter.data;
        }

        if(visual_iter.data->visual_id == screen->root_visual
            and depth_iter.data->depth == screen->root_depth) {
          xcb_root_visual_type = visual_iter.data;
        }
      }
    }
  }
}


void XDesktop::start(VNCServer* vs) {

  // Determine actual number of buttons of the X pointer device.

  auto c = xcb_get_pointer_mapping(xcb);
  auto r = xcb_get_pointer_mapping_reply(xcb, c, nullptr);
  if (not r)
    throw Exception("Cannot get pointer mapping");

  int numButtons = xcb_get_pointer_mapping_map_length(r);
  maxButtons = (numButtons > 8) ? 8 : numButtons;
  vlog.info("Enabling %d button%s of X pointer device",
            maxButtons, (maxButtons != 1) ? "s" : "");
  free(r);

  // Create pixel buffer and provide it to the server object.
  pb = new XPixelBuffer(xcb, xcb_root_visual_type, default_root, geometry.getRect());
//  vlog.info("Allocated %s", pb->getImage()->classDesc());

  server = vs;
  server->setPixelBuffer(pb, computeScreenLayout());

#ifdef HAVE_XDAMAGE
  {
    damage = xcb_generate_id(xcb);
    auto c = xcb_damage_create_checked(xcb, damage, default_root, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);
    auto e = xcb_request_check(xcb, c);
    if (e)
      vlog.error("ERROR");
    vlog.debug("create damage %d", damage);
  }
#endif

#ifdef HAVE_XFIXES
  setCursor();
#endif

  server->setLEDState(ledState);

  running = true;
}

void XDesktop::stop() {
  running = false;

#ifdef HAVE_XDAMAGE
  {
    xcb_damage_destroy(xcb, damage);
    vlog.debug("destroy damage %d", damage);
  }
#endif

  delete queryConnectDialog;
  queryConnectDialog = 0;

  server->setPixelBuffer(0);
  server = 0;

  delete pb;
  pb = 0;
}

void XDesktop::terminate() {
  kill(getpid(), SIGTERM);
}

bool XDesktop::isRunning() {
  return running;
}

void XDesktop::queryConnection(network::Socket* sock,
                               const char* userName)
{
//  assert(isRunning());
//
//  if (queryConnectSock) {
//    server->approveConnection(sock, false, "Another connection is currently being queried.");
//    return;
//  }
//
//  if (!userName)
//    userName = "(anonymous)";
//
//  queryConnectSock = sock;
//
//  CharArray address(sock->getPeerAddress());
//  delete queryConnectDialog;
//  queryConnectDialog = new QueryConnectDialog(dpy, address.buf,
//                                              userName,
//                                              queryConnectTimeout,
//                                              this);
//  queryConnectDialog->map();
}

void XDesktop::pointerEvent(const Point& pos, int buttonMask) {
#ifdef HAVE_XTEST
  if (!haveXtest) return;
  xcb_test_fake_input(xcb, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME,
      default_root,
      geometry.offsetLeft() + pos.x,
      geometry.offsetTop() + pos.y, 0);
  if (buttonMask != oldButtonMask) {
    for (int i = 0; i < maxButtons; i++) {
      if ((buttonMask ^ oldButtonMask) & (1<<i)) {
        if (buttonMask & (1<<i)) {
          xcb_test_fake_input(xcb, XCB_BUTTON_PRESS, i+1, XCB_CURRENT_TIME, default_root, 0, 0, 0);
        } else {
          xcb_test_fake_input(xcb, XCB_BUTTON_RELEASE, i+1, XCB_CURRENT_TIME, default_root, 0, 0, 0);
        }
      }
    }
  }
  oldButtonMask = buttonMask;
#endif
}

#ifdef HAVE_XTEST
KeyCode XDesktop::XkbKeysymToKeycode(Display* dpy, KeySym keysym) {
//  XkbDescPtr xkb;
//  XkbStateRec state;
//  unsigned int mods;
//  unsigned keycode;
//
//  xkb = XkbGetMap(dpy, XkbAllComponentsMask, XkbUseCoreKbd);
//  if (!xkb)
//    return 0;
//
//  XkbGetState(dpy, XkbUseCoreKbd, &state);
//  // XkbStateFieldFromRec() doesn't work properly because
//  // state.lookup_mods isn't properly updated, so we do this manually
//  mods = XkbBuildCoreState(XkbStateMods(&state), state.group);
//
//  for (keycode = xkb->min_key_code;
//       keycode <= xkb->max_key_code;
//       keycode++) {
//    KeySym cursym;
//    unsigned int out_mods;
//    XkbTranslateKeyCode(xkb, keycode, mods, &out_mods, &cursym);
//    if (cursym == keysym)
//      break;
//  }
//
//  if (keycode > xkb->max_key_code)
//    keycode = 0;
//
//  XkbFreeKeyboard(xkb, XkbAllComponentsMask, True);
//
//  // Shift+Tab is usually ISO_Left_Tab, but RFB hides this fact. Do
//  // another attempt if we failed the initial lookup
//  if ((keycode == 0) && (keysym == XK_Tab) && (mods & ShiftMask))
//    return XkbKeysymToKeycode(dpy, XK_ISO_Left_Tab);
//
//  return keycode;
  return 0;
}
#endif

void XDesktop::keyEvent(rdr::U32 keysym, rdr::U32 xtcode, bool down) {
#ifdef HAVE_XTEST
  int keycode = 0;

  if (!haveXtest)
    return;

  // Use scan code if provided and mapping exists
  if (codeMap && rawKeyboard && xtcode < codeMapLen)
      keycode = codeMap[xtcode];

  if (!keycode) {
    if (pressedKeys.find(keysym) != pressedKeys.end())
      keycode = pressedKeys[keysym];
    else {
      // XKeysymToKeycode() doesn't respect state, so we have to use
      // something slightly more complex
      keycode = 0;//XkbKeysymToKeycode(dpy, keysym);
    }
  }

  if (!keycode) {
    vlog.error("Could not map key event to X11 key code");
    return;
  }

  if (down)
    pressedKeys[keysym] = keycode;
  else
    pressedKeys.erase(keysym);

  vlog.debug("keycode = 0x%x %s", keycode, down ? "down" : "up");

  xcb_test_fake_input(xcb, down?XCB_KEY_PRESS:XCB_KEY_RELEASE, keycode, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
#endif
}

void XDesktop::clientCutText(const char* str) {
}

rfb::ScreenSet ZZcomputeScreenLayout(xcb_connection_t * xcb, xcb_window_t root, OutputIdMap &outputIdMap)
{
  rfb::ScreenSet layout;
  OutputIdMap newIdMap;

  /** update root size infos **/
  auto ck0 = xcb_get_geometry(xcb, root);
  auto ck1 = xcb_randr_get_screen_resources(xcb, root);

  // geometry is used as fallback. maybe do not resquest it each time.
  auto * geometry = xcb_get_geometry_reply(xcb, ck0, nullptr);
  auto * randr_resources = xcb_randr_get_screen_resources_reply(xcb, ck1, nullptr);

  if(geometry == nullptr or randr_resources == nullptr) {
    throw Exception("FATAL: cannot read root window attributes");
  }

  std::map<xcb_randr_crtc_t, xcb_randr_get_crtc_info_reply_t *> crtc_info;
  // make request
  std::vector<xcb_randr_get_crtc_info_cookie_t> ckx(xcb_randr_get_screen_resources_crtcs_length(randr_resources));
  xcb_randr_crtc_t * crtc_list = xcb_randr_get_screen_resources_crtcs(randr_resources);
  for (int k = 0; k < xcb_randr_get_screen_resources_crtcs_length(randr_resources); ++k) {
    ckx[k] = xcb_randr_get_crtc_info(xcb, crtc_list[k], XCB_CURRENT_TIME);
  }

  // gather result
  for (int k = 0; k < xcb_randr_get_screen_resources_crtcs_length(randr_resources); ++k) {
    auto * r = xcb_randr_get_crtc_info_reply(xcb, ckx[k], 0);
    if(r != nullptr) {
      crtc_info[crtc_list[k]] = r;

      auto outputId = crtc_list[k];
      auto x = outputIdMap.find(outputId);
      if (x == outputIdMap.end()) {
        // lookup for a new Id
        rdr::U32 id;
        OutputIdMap::const_iterator iter;
        while (true) {
          id = rand();
          for (iter = outputIdMap.begin(); iter != outputIdMap.end(); ++iter) {
            if (iter->second == id)
              break;
          }
          if (iter == outputIdMap.end())
            break;
        }

        newIdMap[outputId] = id;

      } else {
        newIdMap[outputId] = x->second;
      }

      switch (r->rotation) {
      case RR_Rotate_90:
      case RR_Rotate_270:
        std::swap(r->width, r->height);
        break;
      }

      layout.add_screen(rfb::Screen(newIdMap[outputId], r->x, r->y, r->width, r->height, 0));

    }

  }

  /* Only keep the entries that are currently active */
  outputIdMap = newIdMap;

  /*
   * Make sure we have something to display. Hopefully it's just temporary
   * that we have no active outputs...
   */
  if (layout.num_screens() == 0)
    layout.add_screen(rfb::Screen(0, geometry->x, geometry->y, geometry->width, geometry->height, 0));

  return layout;
}

ScreenSet XDesktop::computeScreenLayout()
{
  ScreenSet layout;

#ifdef HAVE_XRANDR
  layout = ::ZZcomputeScreenLayout(xcb, default_root, outputIdMap);

  // Adjust the layout relative to the geometry
  ScreenSet::iterator iter, iter_next;
  Point offset(-geometry.offsetLeft(), -geometry.offsetTop());
  for (iter = layout.begin();iter != layout.end();iter = iter_next) {
    iter_next = iter; ++iter_next;
    iter->dimensions = iter->dimensions.translate(offset);
    if (iter->dimensions.enclosed_by(geometry.getRect()))
        continue;
    iter->dimensions = iter->dimensions.intersect(geometry.getRect());
    if (iter->dimensions.is_empty()) {
      layout.remove_screen(iter->id);
    }
  }
#endif

  // Make sure that we have at least one screen
  if (layout.num_screens() == 0)
    layout.add_screen(rfb::Screen(0, 0, 0, geometry.width(),
                                  geometry.height(), 0));

  return layout;
}

#ifdef HAVE_XRANDR
/* Get the biggest mode which is equal or smaller to requested
   size. If no such mode exists, return the smallest. */
static void GetSmallerMode(XRRScreenResources *res,
                    XRROutputInfo *output,
                    unsigned int *width, unsigned int *height)
{
  XRRModeInfo best = {};
  XRRModeInfo smallest = {};
  smallest.width = -1;
  smallest.height = -1;

  for (int i = 0; i < res->nmode; i++) {
    for (int j = 0; j < output->nmode; j++) {
      if (output->modes[j] == res->modes[i].id) {
        if ((res->modes[i].width > best.width && res->modes[i].width <= *width) &&
            (res->modes[i].height > best.height && res->modes[i].height <= *height)) {
          best = res->modes[i];
        }
        if ((res->modes[i].width < smallest.width) && res->modes[i].height < smallest.height) {
          smallest = res->modes[i];
        }
      }
    }
  }

  if (best.id == 0 && smallest.id != 0) {
    best = smallest;
  }

  *width = best.width;
  *height = best.height;
}
#endif /* HAVE_XRANDR */

unsigned int XDesktop::setScreenLayout(int fb_width, int fb_height,
                                       const rfb::ScreenSet& layout)
{
//#ifdef HAVE_XRANDR
//  char buffer[2048];
//  vlog.debug("Got request for framebuffer resize to %dx%d",
//             fb_width, fb_height);
//  layout.print(buffer, sizeof(buffer));
//  vlog.debug("%s", buffer);
//
//  XRRScreenResources *res = XRRGetScreenResources(dpy, DefaultRootWindow(dpy));
//  if (!res) {
//    vlog.error("XRRGetScreenResources failed");
//    return rfb::resultProhibited;
//  }
//  vncSetGlueContext(dpy, res);
//
//  /* The client may request a screen layout which is not supported by
//     the Xserver. This happens, for example, when adjusting the size
//     of a non-fullscreen vncviewer window. To handle this and other
//     cases, we first call tryScreenLayout. If this fails, we try to
//     adjust the request to one screen with a smaller mode. */
//  vlog.debug("Testing screen layout");
//  unsigned int tryresult = ::tryScreenLayout(fb_width, fb_height, layout, &outputIdMap);
//  rfb::ScreenSet adjustedLayout;
//  if (tryresult == rfb::resultSuccess) {
//    adjustedLayout = layout;
//  } else {
//    vlog.debug("Impossible layout - trying to adjust");
//
//    ScreenSet::const_iterator firstscreen = layout.begin();
//    adjustedLayout.add_screen(*firstscreen);
//    ScreenSet::iterator iter = adjustedLayout.begin();
//    RROutput outputId = None;
//
//    for (int i = 0;i < vncRandRGetOutputCount();i++) {
//      unsigned int oi = vncRandRGetOutputId(i);
//
//      /* Known? */
//      if (outputIdMap.count(oi) == 0)
//        continue;
//
//      /* Find the corresponding screen... */
//      if (iter->id == outputIdMap[oi]) {
//        outputId = oi;
//      } else {
//        outputIdMap.erase(oi);
//      }
//    }
//
//    /* New screen */
//    if (outputId == None) {
//      int i = getPreferredScreenOutput(&outputIdMap, std::set<unsigned int>());
//      if (i != -1) {
//        outputId = vncRandRGetOutputId(i);
//      }
//    }
//    if (outputId == None) {
//      vlog.debug("Resize adjust: Could not find corresponding screen");
//      XRRFreeScreenResources(res);
//      return rfb::resultInvalid;
//    }
//    XRROutputInfo *output = XRRGetOutputInfo(dpy, res, outputId);
//    if (!output) {
//      vlog.debug("Resize adjust: XRRGetOutputInfo failed");
//      XRRFreeScreenResources(res);
//      return rfb::resultInvalid;
//    }
//    if (!output->crtc) {
//      vlog.debug("Resize adjust: Selected output has no CRTC");
//      XRRFreeScreenResources(res);
//      XRRFreeOutputInfo(output);
//      return rfb::resultInvalid;
//    }
//    XRRCrtcInfo *crtc = XRRGetCrtcInfo(dpy, res, output->crtc);
//    if (!crtc) {
//      vlog.debug("Resize adjust: XRRGetCrtcInfo failed");
//      XRRFreeScreenResources(res);
//      XRRFreeOutputInfo(output);
//      return rfb::resultInvalid;
//    }
//
//    unsigned int swidth = iter->dimensions.width();
//    unsigned int sheight = iter->dimensions.height();
//
//    switch (crtc->rotation) {
//    case RR_Rotate_90:
//    case RR_Rotate_270:
//      unsigned int swap = swidth;
//      swidth = sheight;
//      sheight = swap;
//      break;
//    }
//
//    GetSmallerMode(res, output, &swidth, &sheight);
//    XRRFreeOutputInfo(output);
//
//    switch (crtc->rotation) {
//    case RR_Rotate_90:
//    case RR_Rotate_270:
//      unsigned int swap = swidth;
//      swidth = sheight;
//      sheight = swap;
//      break;
//    }
//
//    XRRFreeCrtcInfo(crtc);
//
//    if (sheight != 0 && swidth != 0) {
//      vlog.debug("Adjusted resize request to %dx%d", swidth, sheight);
//      iter->dimensions.setXYWH(0, 0, swidth, sheight);
//      fb_width = swidth;
//      fb_height = sheight;
//    } else {
//      vlog.error("Failed to find smaller or equal screen size");
//      XRRFreeScreenResources(res);
//      return rfb::resultInvalid;
//    }
//  }
//
//  vlog.debug("Changing screen layout");
//  unsigned int ret = ::setScreenLayout(fb_width, fb_height, adjustedLayout, &outputIdMap);
//  XRRFreeScreenResources(res);
//
//  /* Send a dummy event to the root window. When this event is seen,
//     earlier change events (ConfigureNotify and/or CrtcChange) have
//     been processed. An Expose event is used for simplicity; does not
//     require any Atoms, and will not affect other applications. */
//  unsigned long serial = XNextRequest(dpy);
//  XExposeEvent ev = {}; /* zero x, y, width, height, count */
//  ev.type = Expose;
//  ev.display = dpy;
//  ev.window = DefaultRootWindow(dpy);
//  if (XSendEvent(dpy, DefaultRootWindow(dpy), False, ExposureMask, (XEvent*)&ev)) {
//    while (randrSyncSerial < serial) {
//      XFlush(dpy);
//      while (XPending(dpy)) {
//        XEvent ev;
//        XNextEvent(dpy, &ev);
//        handleGlobalEvent(&ev);
//      }
//      XFlush(dpy);
//    }
//  } else {
//    vlog.error("XSendEvent failed");
//  }
//
//  /* The protocol requires that an error is returned if the requested
//     layout could not be set. This is checked by
//     VNCSConnectionST::setDesktopSize. Another ExtendedDesktopSize
//     with reason=0 will be sent in response to the changes seen by the
//     event handler. */
//  if (adjustedLayout != layout)
//    return rfb::resultInvalid;
//
//  // Explicitly update the server state with the result as there
//  // can be corner cases where we don't get feedback from the X server
//  server->setScreenLayout(computeScreenLayout());
//
//  return ret;
//
//#else
//  return rfb::resultProhibited;
//#endif /* HAVE_XRANDR */
  return rfb::resultProhibited;
}


bool XDesktop::handleGlobalEvent(xcb_generic_event_t* ev) {
  vlog.debug("XEvent %d", ev->response_type);
  if (ev->response_type == xkbEventBase) {
    if (reinterpret_cast<xcb_xkb_new_keyboard_notify_event_t*>(ev)->xkbType != XCB_XKB_INDICATOR_STATE_NOTIFY)
      return false;

    auto const * kb = reinterpret_cast<xcb_xkb_indicator_state_notify_event_t*>(ev);

    vlog.debug("Got indicator update, mask is now 0x%x", kb->state);

    ledState = 0;
    for (int i = 0; i < XDESKTOP_N_LEDS; i++) {
      if (kb->state & ledMasks[i])
        ledState |= 1u << i;
    }

    if (running)
      server->setLEDState(ledState);

    return true;
#ifdef HAVE_XDAMAGE
  } else if (ev->response_type == (xdamageEventBase + XCB_DAMAGE_NOTIFY)) {
    vlog.debug("Damage notify");
    auto const * dev = reinterpret_cast<xcb_damage_notify_event_t*>(ev);
    Rect rect;

    if (!running)
      return true;

    rect.setXYWH(dev->area.x, dev->area.y, dev->area.width, dev->area.height);
    rect = rect.translate(Point(-geometry.offsetLeft(), -geometry.offsetTop()));
    server->add_changed(rect);

    return true;
#endif
#ifdef HAVE_XFIXES
  } else if (ev->response_type == (xfixesEventBase + XCB_XFIXES_CURSOR_NOTIFY)) {
    auto const * cev = reinterpret_cast<xcb_xfixes_cursor_notify_event_t*>(ev);

    if (!running)
      return true;

    if (cev->subtype != XCB_XFIXES_CURSOR_NOTIFY_DISPLAY_CURSOR)
      return false;

    return setCursor();
#endif
#ifdef HAVE_XRANDR
  } else if (ev->response_type == XCB_EXPOSE) {
    auto const * eev = reinterpret_cast<xcb_expose_event_t*>(ev);
    randrSyncSerial = eev->sequence;

    return false;

  } else if (ev->response_type == ConfigureNotify) {
    auto const * cev = reinterpret_cast<xcb_configure_notify_event_t*>(ev);

    if (cev->window != default_root) {
      return false;
    }

//    XRRUpdateConfiguration(ev);
    geometry.recalc(cev->width, cev->height);

    if (!running) {
      return false;
    }

    if ((cev->width != pb->width() || (cev->height != pb->height()))) {
      delete pb;
      pb = new XPixelBuffer(xcb, xcb_root_visual_type, default_root, geometry.getRect());
      server->setPixelBuffer(pb, computeScreenLayout());

      // Mark entire screen as changed
      server->add_changed(rfb::Region(Rect(0, 0, cev->width, cev->height)));
    }

    return true;

  } else if (ev->response_type == (xrandrEventBase + XCB_RANDR_NOTIFY)) {
    auto const * rev = reinterpret_cast<xcb_randr_notify_event_t*>(ev);

    if (!running)
      return false;

    if (rev->subCode == XCB_RANDR_NOTIFY_CRTC_CHANGE) {

      if (rev->u.cc.window != default_root) {
        return false;
      }

      server->setScreenLayout(computeScreenLayout());
    }

    return true;
#endif
  }

  return false;
}

void XDesktop::queryApproved()
{
  assert(isRunning());
  server->approveConnection(queryConnectSock, true, 0);
  queryConnectSock = 0;
}

void XDesktop::queryRejected()
{
  assert(isRunning());
  server->approveConnection(queryConnectSock, false,
                            "Connection rejected by local user");
  queryConnectSock = 0;
}

bool XDesktop::setCursor()
{
  xcb_generic_error_t * e;
  auto c = xcb_xfixes_get_cursor_image(xcb);
  auto cim = xcb_xfixes_get_cursor_image_reply(xcb, c, &e);
  if (cim == nullptr) {
    vlog.debug("Cannot get the cursor image");
    return false;
  }

  // Copied from XserverDesktop::setCursor() in
  // unix/xserver/hw/vnc/XserverDesktop.cc and adapted to
  // handle long -> U32 conversion for 64-bit Xlib
  rdr::U8* cursorData;
  rdr::U8 *out;
  const uint32_t *pixels;

  cursorData = new rdr::U8[cim->width * cim->height * 4];

  // Un-premultiply alpha
  pixels = xcb_xfixes_get_cursor_image_cursor_image(cim);
  out = cursorData;
  for (int y = 0; y < cim->height; y++) {
    for (int x = 0; x < cim->width; x++) {
      rdr::U8 alpha;
      rdr::U32 pixel = *pixels++;

      alpha = (pixel >> 24) & 0xff;
      if (alpha == 0)
        alpha = 1; // Avoid division by zero

      *out++ = ((pixel >> 16) & 0xff) * 255/alpha;
      *out++ = ((pixel >>  8) & 0xff) * 255/alpha;
      *out++ = ((pixel >>  0) & 0xff) * 255/alpha;
      *out++ = ((pixel >> 24) & 0xff);
    }
  }

  try {
    server->setCursor(cim->width, cim->height, Point(cim->xhot, cim->yhot),
                      cursorData);
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::setCursor: %s",e.str());
  }

  delete [] cursorData;
  free(cim);
  return true;
}
