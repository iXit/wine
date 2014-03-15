/*
 * Wine X11DRV DRI2 interface
 *
 * Copyright 2013 Joakim Sindholt
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_DRI2_H
#define __WINE_DRI2_H

#ifndef __WINE_CONFIG_H
# error You must include config.h to use this header
#endif

#if defined(SONAME_LIBXEXT) && defined(SONAME_LIBXFIXES)

#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/dri2tokens.h>
#include <X11/extensions/dri2proto.h>

#include <stdint.h>

typedef struct
{
   unsigned attachment;
   unsigned name;
   unsigned pitch;
   unsigned cpp;
   unsigned flags;
} DRI2Buffer;

Bool
DRI2QueryExtension( Display * dpy/* , int *event_basep, int *error_basep */ );

Bool
DRI2QueryVersion( Display *dpy,
                  unsigned *major,
                  unsigned *minor );

Bool
DRI2Connect( Display *dpy,
             XID window,
             unsigned driver_type,
             char **driver,
             char **device );

Bool
DRI2Authenticate( Display *dpy,
                  XID window,
                  uint32_t token );

void
DRI2CreateDrawable( Display *dpy,
                    XID drawable );

void
DRI2DestroyDrawable( Display *dpy,
                     XID drawable );

unsigned
DRI2GetBuffers( Display *dpy,
                XID drawable,
                const unsigned *attachments,
                unsigned attach_count,
                unsigned *width,
                unsigned *height,
                DRI2Buffer **buffers );

unsigned
DRI2GetBuffersWithFormat( Display *dpy,
                          XID drawable,
                          const unsigned *attachments,
                          unsigned attach_count,
                          unsigned *width,
                          unsigned *height,
                          DRI2Buffer **buffers );

Bool
DRI2CopyRegion( Display *dpy,
                XID drawable,
                XserverRegion region,
                unsigned dest,
                unsigned src );

#endif /* defined(SONAME_LIBXEXT) && defined(SONAME_LIBXFIXES) */

#endif /* __WINE_DRI2_H */
