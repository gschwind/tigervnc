/* Copyright (C) 2007-2008 Constantin Kaplinsky.  All Rights Reserved.
 * Copyright 2014 Pierre Ossman for Cendio AB
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
// XPixelBuffer.cxx
//

#include <vector>
#include <rfb/Region.h>
#include <spawnvncserver/XPixelBuffer.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>

using namespace rfb;

XPixelBuffer::XPixelBuffer(xcb_connection_t *xcb, xcb_visualtype_t * visual, xcb_drawable_t d, const rfb::Rect &rect)
  : FullFramePixelBuffer(),
    m_xcb(xcb),
    m_visual(visual),
    m_offsetLeft(rect.tl.x),
    m_offsetTop(rect.tl.y)
{

  // Fill in the PixelFormat structure of the parent class.
  format = PixelFormat(
      32, // bits per pixel
      24, // depth
      false/*is bigendian?*/, true/*truecolor*/,
      m_visual->red_mask >> (ffs(m_visual->red_mask) - 1),
      m_visual->green_mask >> (ffs(m_visual->green_mask) - 1),
      m_visual->blue_mask >> (ffs(m_visual->blue_mask) - 1),
      ffs(m_visual->red_mask) - 1,
      ffs(m_visual->green_mask) - 1,
      ffs(m_visual->blue_mask) - 1);

  // Set up the remaining data of the parent class.
  width_ = rect.width();
  height_ = rect.height();

  int cairo_stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width_);

  // Calculate the distance in pixels between two subsequent scan
  // lines of the framebuffer. This may differ from image width.
  stride = cairo_stride/4;

  data = new rdr::U8[cairo_stride*height_];

  cairo_status_t status;

  m_surf_frame_bufer = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, width_, height_, cairo_stride);
  status = cairo_surface_status(m_surf_frame_bufer);
  if (status != CAIRO_STATUS_SUCCESS)
    printf("Cairo fail %d : %s\n", __LINE__, cairo_status_to_string(status));

  m_surf_xcb_root = cairo_xcb_surface_create(xcb, d, m_visual, width_, height_);
  status = cairo_surface_status(m_surf_xcb_root);
  if (status != CAIRO_STATUS_SUCCESS)
    printf("Cairo fail %d : %s\n", __LINE__, cairo_status_to_string(status));

}

XPixelBuffer::~XPixelBuffer()
{
  cairo_surface_destroy(m_surf_frame_bufer);
  cairo_surface_destroy(m_surf_xcb_root);
  delete[] data;
}

void
XPixelBuffer::grabRegion(const rfb::Region& region)
{
  printf("Entering grabRegion\n");
  std::vector<Rect> rects;
  std::vector<Rect>::const_iterator i;
  region.get_rects(&rects);

  // this seems to be required to flush root window to ensure sync.
  cairo_surface_flush(m_surf_xcb_root);
  cairo_t * cr = cairo_create(m_surf_frame_bufer);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  for (i = rects.begin(); i != rects.end(); i++) {
    // Copy pixels from the screen to the pixel buffer,
    // for the specified rectangular area of the buffer.
    auto const &r = *i;
    printf("%d %d %d %d\n", r.tl.x, r.tl.y, r.width(), r.height());
    cairo_set_source_surface(cr, m_surf_xcb_root, 0, 0);
    cairo_rectangle(cr, r.tl.x, r.tl.y, r.width(), r.height());
    cairo_fill(cr);
    cairo_status_t status = cairo_status(cr);
    if (status != CAIRO_STATUS_SUCCESS)
      printf("Cairo fail %d : %s\n", __LINE__, cairo_status_to_string(status));

    cairo_set_source_rgb(cr, 0.0, 0.5, 0.0);
    cairo_rectangle(cr, r.tl.x+0.5, r.tl.y+0.5, r.width()-1, r.height()-1);
    cairo_stroke(cr);
  }

  cairo_destroy(cr);
  cairo_surface_flush(m_surf_frame_bufer);
  printf("Leaving grabRegion\n");
}

