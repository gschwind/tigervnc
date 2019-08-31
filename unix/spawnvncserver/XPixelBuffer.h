/* Copyright (C) 2007-2008 Constantin Kaplinsky.  All Rights Reserved.
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

//
// XPixelBuffer.h
//

#ifndef __XPIXELBUFFER_H__
#define __XPIXELBUFFER_H__

#include <rfb/PixelBuffer.h>
#include <rfb/VNCServer.h>

#include <xcb/xcb.h>
#include <cairo/cairo.h>

//
// XPixelBuffer is an Image-based implementation of FullFramePixelBuffer.
//

class XPixelBuffer : public rfb::FullFramePixelBuffer
{
public:
  XPixelBuffer(xcb_connection_t *xcb, xcb_visualtype_t * visual, xcb_drawable_t d, const rfb::Rect &rect);
  virtual ~XPixelBuffer();

  // Override PixelBuffer::grabRegion().
  virtual void grabRegion(const rfb::Region& region);

protected:
  xcb_connection_t * m_xcb;
  xcb_visualtype_t * m_visual;

  cairo_surface_t * m_surf_frame_bufer;
  cairo_surface_t * m_surf_xcb_root;

  int m_offsetLeft;
  int m_offsetTop;

  // Copy pixels from the screen to the pixel buffer,
  // for the specified rectangular area of the buffer.
  inline void grabRect(const rfb::Rect &r) {
    cairo_t * cr = cairo_create(m_surf_frame_bufer);
    cairo_set_source_surface(cr, m_surf_xcb_root, -m_offsetLeft+r.tl.x, -m_offsetTop +r.tl.y);
    cairo_rectangle(cr, r.tl.x, r.tl.y, r.width(), r.height());
    cairo_paint(cr);
  }
};

#endif // __XPIXELBUFFER_H__

