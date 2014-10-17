/*
 * Wine X11DRV DRI3 interface
 *
 * Copyright 2014 Axel Davy
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


#include "config.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

#if defined(SONAME_LIBXEXT) && defined(SONAME_LIBXFIXES)

#include "x11drv.h"
#include "wine/d3dadapter.h"

#include <stdlib.h>
#include <fcntl.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <X11/Xlib-xcb.h>

#include "xfixes.h"
#include "dri3.h"
#include <pthread.h>
#include "winbase.h" /* for Sleep */


BOOL
DRI3CheckExtension(Display *dpy, int major, int minor)
{
    xcb_connection_t *xcb_connection = XGetXCBConnection(dpy);
    xcb_dri3_query_version_cookie_t dri3_cookie;
    xcb_dri3_query_version_reply_t *dri3_reply;
    xcb_generic_error_t *error;
    const xcb_query_extension_reply_t *extension;

    xcb_prefetch_extension_data(xcb_connection, &xcb_dri3_id);

    extension = xcb_get_extension_data(xcb_connection, &xcb_dri3_id);
    if (!(extension && extension->present)) {
        TRACE("DRI3 extension is not present\n");
        return FALSE;
    }

    dri3_cookie = xcb_dri3_query_version(xcb_connection, major, minor);

    dri3_reply = xcb_dri3_query_version_reply(xcb_connection, dri3_cookie, &error);
    if (!dri3_reply) {
        free(error);
        TRACE("Issue getting requested version of DRI3: %d,%d\n", major, minor);
        return FALSE;
    }

    TRACE("DRI3 version %d,%d found. %d %d requested\n", major, minor, (int)dri3_reply->major_version, (int)dri3_reply->minor_version);
    free(dri3_reply);

    return TRUE;
}

BOOL
PRESENTCheckExtension(Display *dpy, int major, int minor)
{
    xcb_connection_t *xcb_connection = XGetXCBConnection(dpy);
    xcb_present_query_version_cookie_t present_cookie;
    xcb_present_query_version_reply_t *present_reply;
    xcb_generic_error_t *error;
    const xcb_query_extension_reply_t *extension;

    xcb_prefetch_extension_data(xcb_connection, &xcb_present_id);

    extension = xcb_get_extension_data(xcb_connection, &xcb_present_id);
    if (!(extension && extension->present)) {
        TRACE("PRESENT extension is not present\n");
        return FALSE;
    }

    present_cookie = xcb_present_query_version(xcb_connection, major, minor);

    present_reply = xcb_present_query_version_reply(xcb_connection, present_cookie, &error);
    if (!present_reply) {
        free(error);
        TRACE("Issue getting requested version of PRESENT: %d,%d\n", major, minor);
        return FALSE;
    }

    TRACE("PRESENT version %d,%d found. %d %d requested\n", major, minor, (int)present_reply->major_version, (int)present_reply->minor_version);
    free(present_reply);

    return TRUE;
}

BOOL
DRI3Open(Display *dpy, int screen, int *device_fd)
{
    xcb_dri3_open_cookie_t cookie;
    xcb_dri3_open_reply_t *reply;
    xcb_connection_t *xcb_connection = XGetXCBConnection(dpy);
    int fd;
    Window root = RootWindow(dpy, screen);

    cookie = xcb_dri3_open(xcb_connection, root, 0);

    reply = xcb_dri3_open_reply(xcb_connection, cookie, NULL);
    if (!reply)
        return FALSE;

    if (reply->nfd != 1) {
        free(reply);
        return FALSE;
    }

    fd = xcb_dri3_open_reply_fds(xcb_connection, reply)[0];
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    *device_fd = fd;

    return TRUE;
}

BOOL
DRI3PixmapFromDmaBuf(Display *dpy, int screen, int fd, int width, int height, int stride, int depth, int bpp, Pixmap *pixmap)
{
    xcb_connection_t *xcb_connection = XGetXCBConnection(dpy);
    Window root = RootWindow(dpy, screen);
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;

    cookie = xcb_dri3_pixmap_from_buffer_checked(xcb_connection,
                                                (*pixmap = xcb_generate_id(xcb_connection)),
                                                root,
                                                0,
                                                width, height, stride,
                                                depth, bpp, fd);
    error = xcb_request_check(xcb_connection, cookie); /* performs a flush */
    if (error) {
        ERR("Error using DRI3 to convert a DmaBufFd to pixmap\n");
        return FALSE;
    }
    return TRUE;
}

BOOL
DRI3DmaBufFromPixmap(Display *dpy, Pixmap pixmap, int *fd, int *width, int *height, int *stride, int *depth, int *bpp)
{
    xcb_connection_t *xcb_connection = XGetXCBConnection(dpy);
    xcb_dri3_buffer_from_pixmap_cookie_t bp_cookie;
    xcb_dri3_buffer_from_pixmap_reply_t  *bp_reply;

    bp_cookie = xcb_dri3_buffer_from_pixmap(xcb_connection, pixmap);
    bp_reply = xcb_dri3_buffer_from_pixmap_reply(xcb_connection, bp_cookie, NULL);
    if (!bp_reply)
        return FALSE;
    *fd = xcb_dri3_buffer_from_pixmap_reply_fds(xcb_connection, bp_reply)[0];
    *width = bp_reply->width;
    *height = bp_reply->height;
    *stride = bp_reply->stride;
    *depth = bp_reply->depth;
    *bpp = bp_reply->depth;
    return TRUE;
}

struct PRESENTPriv {
    xcb_connection_t *xcb_connection;
    XID window;
    uint64_t last_msc;
    uint64_t last_target;
    uint32_t last_serial;
    xcb_special_event_t *special_event;
    PRESENTPixmapPriv *first_present_priv;
    int pixmap_present_pending;
    BOOL notify_with_serial_pending;
    pthread_mutex_t mutex_present; /* protect readind/writing present_priv things */
    pthread_mutex_t mutex_xcb_wait;
    BOOL xcb_wait;
};

struct PRESENTPixmapPriv {
    PRESENTpriv *present_priv;
    Pixmap pixmap;
    BOOL released;
    BOOL present_complete_pending;
    uint32_t present_pending_serial;
    BOOL last_present_was_flip;
    PRESENTPixmapPriv *next;
};

static PRESENTPixmapPriv *PRESENTFindPixmapPriv(PRESENTpriv *present_priv, uint32_t serial)
{
    PRESENTPixmapPriv *current = present_priv->first_present_priv;

    while (current) {
        if (current->present_pending_serial == serial)
            return current;
        current = current->next;
    }
    return NULL;
}

static void PRESENThandle_events(PRESENTpriv *present_priv, xcb_present_generic_event_t *ge)
{
    PRESENTPixmapPriv *present_pixmap_priv = NULL;

    switch (ge->evtype) {
        case XCB_PRESENT_COMPLETE_NOTIFY: {
            xcb_present_complete_notify_event_t *ce = (void *) ge;
            if (ce->kind == XCB_PRESENT_COMPLETE_KIND_NOTIFY_MSC) {
                if (ce->serial)
                    present_priv->notify_with_serial_pending = FALSE;
                free(ce);
                return;
            }
            present_pixmap_priv = PRESENTFindPixmapPriv(present_priv, ce->serial);
            if (!present_pixmap_priv || ce->kind != XCB_PRESENT_COMPLETE_KIND_PIXMAP) {
                ERR("FATAL ERROR: PRESENT handling failed\n");
                free(ce);
                return;
            }
            present_pixmap_priv->present_complete_pending = FALSE;
            switch (ce->mode) {
                case XCB_PRESENT_COMPLETE_MODE_FLIP:
                    present_pixmap_priv->last_present_was_flip = TRUE;
                    break;
                case XCB_PRESENT_COMPLETE_MODE_COPY:
                    present_pixmap_priv->last_present_was_flip = FALSE;
                    break;
            }
            if (present_pixmap_priv->released)
                present_pixmap_priv->present_pending_serial = 0;

            present_priv->pixmap_present_pending--;
            present_priv->last_msc = ce->msc;
            break;
        }
        case XCB_PRESENT_EVENT_IDLE_NOTIFY: {
            xcb_present_idle_notify_event_t *ie = (void *) ge;
            present_pixmap_priv = PRESENTFindPixmapPriv(present_priv, ie->serial);
            if (!present_pixmap_priv || present_pixmap_priv->pixmap != ie->pixmap) {
                ERR("FATAL ERROR: PRESENT handling failed\n");
                free(ie);
                return;
            }
            present_pixmap_priv->released = TRUE;
            if (!present_pixmap_priv->present_complete_pending)
                present_pixmap_priv->present_pending_serial = 0;
            break;
        }
    }
    free(ge);
}

static void PRESENTflush_events(PRESENTpriv *present_priv, BOOL assert_no_other_thread_waiting)
{
    xcb_generic_event_t *ev;

    if ((present_priv->xcb_wait && !assert_no_other_thread_waiting) || /* don't steal events to someone waiting */
        !present_priv->special_event)
        return;

    while ((ev = xcb_poll_for_special_event(present_priv->xcb_connection, present_priv->special_event)) != NULL) {
        PRESENThandle_events(present_priv, (void *) ev);
    }
}

static BOOL PRESENTwait_events(PRESENTpriv *present_priv, BOOL allow_other_threads)
{
    xcb_generic_event_t *ev;

    if (allow_other_threads) {
        present_priv->xcb_wait = TRUE;
        pthread_mutex_lock(&present_priv->mutex_xcb_wait);
        pthread_mutex_unlock(&present_priv->mutex_present);
    }
    ev = xcb_wait_for_special_event(present_priv->xcb_connection, present_priv->special_event);
    if (allow_other_threads) {
        pthread_mutex_unlock(&present_priv->mutex_xcb_wait);
        pthread_mutex_lock(&present_priv->mutex_present);
        present_priv->xcb_wait = FALSE;
    }
    if (!ev) {
        ERR("FATAL error: xcb had an error\n");
        return FALSE;
    }

    PRESENThandle_events(present_priv, (void *) ev);
    return TRUE;
}

BOOL
PRESENTInit(Display *dpy, PRESENTpriv **present_priv)
{
    *present_priv = (PRESENTpriv *) calloc(1, sizeof(PRESENTpriv));
    if (!*present_priv) {
        return FALSE;
    }
    (*present_priv)->xcb_connection = XGetXCBConnection(dpy);
    pthread_mutex_init(&(*present_priv)->mutex_present, NULL);
    pthread_mutex_init(&(*present_priv)->mutex_xcb_wait, NULL);
    return TRUE;
}

static void PRESENTForceReleases(PRESENTpriv *present_priv)
{
    PRESENTPixmapPriv *current = NULL;

    if (!present_priv->window)
        return;

    /* There should be no other thread listening for events here.
     * This can happen when hDestWindowOverride changes without reset.
     * This case should never happen, but can happen in theory.*/
    if (present_priv->xcb_wait) {
        xcb_present_notify_msc(present_priv->xcb_connection, present_priv->window, 0, 0, 0, 0);
        xcb_flush(present_priv->xcb_connection);
        pthread_mutex_lock(&present_priv->mutex_xcb_wait);
        pthread_mutex_unlock(&present_priv->mutex_xcb_wait);
        /* the problem here is that we don't have access to the event the other thread got.
         * It is either presented event, idle event or notify event.
         */
        while (present_priv->pixmap_present_pending >= 1)
            PRESENTwait_events(present_priv, FALSE);
        PRESENTflush_events(present_priv, TRUE);
        /* Remaining events to come can be a pair of present/idle,
         * or an idle, or nothing. To be sure we are after all pixmaps
         * have been presented, add an event to the queue that can only
         * be after the present event, then if we receive an event more,
         * we are sure all pixmaps were presented */
        present_priv->notify_with_serial_pending = TRUE;
        xcb_present_notify_msc(present_priv->xcb_connection, present_priv->window, 1, present_priv->last_target + 5, 0, 0);
        xcb_flush(present_priv->xcb_connection);
        while (present_priv->notify_with_serial_pending)
            PRESENTwait_events(present_priv, FALSE);
        /* Now we are sure we are not expecting any new event */
    } else {
        while (present_priv->pixmap_present_pending) /* wait all sent pixmaps are presented */
            PRESENTwait_events(present_priv, FALSE);
        PRESENTflush_events(present_priv, TRUE); /* may be remaining idle event */
        /* Since idle events are send with the complete events when it is not flips,
         * we are not expecting any new event here */
    }

    current = present_priv->first_present_priv;
    while (current) {
        if (!current->released) {
            if (!current->last_present_was_flip && !present_priv->xcb_wait) {
                ERR("ERROR: a pixmap seems not released by PRESENT for no reason. Code bug.\n");
            } else {
                /* Present the same pixmap with a non-valid part to force the copy mode and the releases */
                xcb_xfixes_region_t valid, update;
                xcb_rectangle_t rect_update;
                rect_update.x = 0;
                rect_update.y = 0;
                rect_update.width = 8;
                rect_update.height = 1;
                valid = xcb_generate_id(present_priv->xcb_connection);
                update = xcb_generate_id(present_priv->xcb_connection);
                xcb_xfixes_create_region(present_priv->xcb_connection, valid, 1, &rect_update);
                xcb_xfixes_create_region(present_priv->xcb_connection, update, 1, &rect_update);
                /* here we know the pixmap has been presented. Thus if it is on screen,
                 * the following request can only make it released by the server if it is not */
                xcb_present_pixmap(present_priv->xcb_connection, present_priv->window,
                                   current->pixmap, 0, valid, update, 0, 0, None, None,
                                   None, XCB_PRESENT_OPTION_COPY | XCB_PRESENT_OPTION_ASYNC, 0, 0, 0, 0, NULL);
                xcb_flush(present_priv->xcb_connection);
                PRESENTwait_events(present_priv, FALSE); /* by assumption this can only be idle event */
                PRESENTflush_events(present_priv, TRUE); /* Shoudln't be needed */
            }
        }
        current = current->next;
    }
    /* Now all pixmaps are released (possibility if xcb_wait is true that one is not aware yet),
     * and we don't expect any new Present event to come from Xserver */
}

static void PRESENTFreeXcbQueue(PRESENTpriv *present_priv)
{
    if (present_priv->window) {
        xcb_unregister_for_special_event(present_priv->xcb_connection, present_priv->special_event);
        present_priv->last_msc = 0;
        present_priv->last_target = 0;
        present_priv->last_serial = 0;
        present_priv->special_event = NULL;
    }
}

static BOOL PRESENTPrivChangeWindow(PRESENTpriv *present_priv, XID window)
{
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;
    xcb_present_event_t eid;

    PRESENTForceReleases(present_priv);
    PRESENTFreeXcbQueue(present_priv);
    present_priv->window = window;

    if (window) {
        cookie = xcb_present_select_input_checked(present_priv->xcb_connection,
                                                  (eid = xcb_generate_id(present_priv->xcb_connection)),
                                                  window,
                                                  XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY|
                                                  XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
        present_priv->special_event = xcb_register_for_special_xge(present_priv->xcb_connection,
                                                                   &xcb_present_id,
                                                                   eid, NULL);
        error = xcb_request_check(present_priv->xcb_connection, cookie); /* performs a flush */
        if (error || !present_priv->special_event) {
            ERR("FAILED to use the X PRESENT extension. Was the destination a window ?\n");
            if (present_priv->special_event)
                xcb_unregister_for_special_event(present_priv->xcb_connection, present_priv->special_event);
            present_priv->special_event = NULL;
            present_priv->window = 0;
        }
    }
    return (present_priv->window != 0);
}

void
PRESENTDestroy(Display *dpy, PRESENTpriv *present_priv)
{
    PRESENTPixmapPriv *current = NULL;

    pthread_mutex_lock(&present_priv->mutex_present);

    PRESENTForceReleases(present_priv);

    current = present_priv->first_present_priv;
    while (current) {
        PRESENTPixmapPriv *next = current->next;
        XFreePixmap(dpy, current->pixmap);
        free(current);
        current = next;
    }

    PRESENTFreeXcbQueue(present_priv);

    pthread_mutex_unlock(&present_priv->mutex_present);
    pthread_mutex_destroy(&present_priv->mutex_present);
    pthread_mutex_destroy(&present_priv->mutex_xcb_wait);

    free(present_priv);
}

BOOL
PRESENTPixmapInit(PRESENTpriv *present_priv, Pixmap pixmap, PRESENTPixmapPriv **present_pixmap_priv)
{
    *present_pixmap_priv = (PRESENTPixmapPriv *) calloc(1, sizeof(PRESENTPixmapPriv));
    if (!*present_pixmap_priv) {
        return FALSE;
    }
    pthread_mutex_lock(&present_priv->mutex_present);
    (*present_pixmap_priv)->released = TRUE;
    (*present_pixmap_priv)->pixmap = pixmap;
    (*present_pixmap_priv)->present_priv = present_priv;
    (*present_pixmap_priv)->next = present_priv->first_present_priv;
    present_priv->first_present_priv = *present_pixmap_priv;
    pthread_mutex_unlock(&present_priv->mutex_present);
    return TRUE;
}

BOOL
PRESENTTryFreePixmap(PRESENTPixmapPriv *present_pixmap_priv)
{
    PRESENTpriv *present_priv = present_pixmap_priv->present_priv;
    PRESENTPixmapPriv *current;

    pthread_mutex_lock(&present_priv->mutex_present);

    if (!present_pixmap_priv->released) {
        pthread_mutex_unlock(&present_priv->mutex_present);
        return FALSE;
    }

    if (present_priv->first_present_priv == present_pixmap_priv) {
        present_priv->first_present_priv = present_pixmap_priv->next;
        goto free_priv;
    }

    current = present_priv->first_present_priv;
    while (current->next != present_pixmap_priv)
        current = current->next;
    current->next = present_pixmap_priv->next;
free_priv:
    free(present_pixmap_priv);
    pthread_mutex_unlock(&present_priv->mutex_present);
    return TRUE;
}

BOOL
PRESENTHelperCopyFront(Display *dpy, PRESENTPixmapPriv *present_pixmap_priv)
{
    PRESENTpriv *present_priv = present_pixmap_priv->present_priv;
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;
    Window root_return;
    int x_return, y_return;
    unsigned int width_return, height_return;
    unsigned int border_width_return;
    unsigned int depth_return;
    uint32_t v;
    xcb_gcontext_t gc;

    pthread_mutex_lock(&present_priv->mutex_present);

    if (!present_priv->window) {
        pthread_mutex_unlock(&present_priv->mutex_present);
        return FALSE;
    }
    XGetGeometry(dpy, present_pixmap_priv->pixmap, &root_return, &x_return, &y_return, &width_return,
                 &height_return, &border_width_return, &depth_return);
    v = 0;
    xcb_create_gc(present_priv->xcb_connection,
                  (gc = xcb_generate_id(present_priv->xcb_connection)),
                  present_priv->window,
                  XCB_GC_GRAPHICS_EXPOSURES,
                  &v);
    cookie = xcb_copy_area_checked(present_priv->xcb_connection,
                                   present_priv->window,
                                   present_pixmap_priv->pixmap,
                                   gc,
                                   0, 0, 0, 0,
                                   width_return, height_return);
    error = xcb_request_check(present_priv->xcb_connection, cookie);
    xcb_free_gc(present_priv->xcb_connection, gc);
    pthread_mutex_unlock(&present_priv->mutex_present);
    return (error != NULL);
}

BOOL
PRESENTPixmap(Display *dpy, XID window,
              PRESENTPixmapPriv *present_pixmap_priv, D3DPRESENT_PARAMETERS *pPresentationParameters,
              const RECT *pSourceRect, const RECT *pDestRect, const RGNDATA *pDirtyRegion)
{
    PRESENTpriv *present_priv = present_pixmap_priv->present_priv;
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;
    int64_t target_msc, presentationInterval;
    int32_t serial;
    xcb_xfixes_region_t valid, update;
    int16_t x_off, y_off;
    uint32_t options = XCB_PRESENT_OPTION_NONE;

    pthread_mutex_lock(&present_priv->mutex_present);

    if (window != present_priv->window)
        PRESENTPrivChangeWindow(present_priv, window);

    if (!window) {
        ERR("ERROR: Try to Present a pixmap on a NULL window\n");
        pthread_mutex_unlock(&present_priv->mutex_present);
        return FALSE;
    }

    PRESENTflush_events(present_priv, FALSE);

    /* workaround an libxcb bug: xcb_request_check won't work
     * if a thread is listening the special queue. So wake the
     * other thread by provoking an event */
    if (present_priv->xcb_wait) {
        xcb_present_notify_msc(present_priv->xcb_connection, present_priv->window, 0, 0, 0, 0);
        xcb_flush(present_priv->xcb_connection);
        pthread_mutex_lock(&present_priv->mutex_xcb_wait);
        pthread_mutex_unlock(&present_priv->mutex_xcb_wait);
    }

    target_msc = present_priv->last_msc;
    switch(pPresentationParameters->PresentationInterval) {
        case D3DPRESENT_INTERVAL_DEFAULT:
        case D3DPRESENT_INTERVAL_ONE:
            presentationInterval = 1;
            break;
        case D3DPRESENT_INTERVAL_TWO:
            presentationInterval = 2;
            break;
        case D3DPRESENT_INTERVAL_THREE:
            presentationInterval = 3;
            break;
        case D3DPRESENT_INTERVAL_FOUR:
            presentationInterval = 4;
            break;
        case D3DPRESENT_INTERVAL_IMMEDIATE:
        default:
            presentationInterval = 0;
            options |= XCB_PRESENT_OPTION_ASYNC;
            break;
    }
    target_msc += presentationInterval * (present_priv->pixmap_present_pending + 1);
    serial = present_priv->last_serial + 1;

    /* Note: PRESENT defines some way to do partial copy:
     * presentproto:
     * 'x-off' and 'y-off' define the location in the window where
     *  the 0,0 location of the pixmap will be presented. valid-area
     *  and update-area are relative to the pixmap.
     */
    if (!pSourceRect && !pDestRect && !pDirtyRegion) {
        valid = 0;
        update = 0;
        x_off = 0;
        y_off = 0;
    } else {
        xcb_rectangle_t rect_update;
        xcb_rectangle_t *rect_updates;
        /* Get pixmap size */
        Window root_return;
        int x_return, y_return;
        unsigned int width_return, height_return;
        unsigned int border_width_return;
        unsigned int depth_return;
        int i;
        XGetGeometry(dpy, present_pixmap_priv->pixmap, &root_return, &x_return, &y_return, &width_return,
                     &height_return, &border_width_return, &depth_return);
        rect_update.x = 0;
        rect_update.y = 0;
        rect_update.width = width_return;
        rect_update.height = height_return;
        x_off = 0;
        y_off = 0;
        if (pSourceRect) {
            x_off = -pSourceRect->left;
            y_off = -pSourceRect->top;
            rect_update.x = pSourceRect->left;
            rect_update.y = pSourceRect->top;
            rect_update.width = pSourceRect->right - pSourceRect->left;
            rect_update.height = pSourceRect->bottom - pSourceRect->top;
        }
        if (pDestRect) {
            x_off += pDestRect->left;
            y_off += pDestRect->top;
            rect_update.width = pDestRect->right - pDestRect->left;
            rect_update.height = pDestRect->bottom - pDestRect->top;
            /* Note: the size of pDestRect and pSourceRect are supposed to be the same size
             * because the driver would have done things to assure that. */
        }
        valid = xcb_generate_id(present_priv->xcb_connection);
        update = xcb_generate_id(present_priv->xcb_connection);
        xcb_xfixes_create_region(present_priv->xcb_connection, valid, 1, &rect_update);
        if (pDirtyRegion && pDirtyRegion->rdh.nCount) {
            rect_updates = (void *) calloc(pDirtyRegion->rdh.nCount, sizeof(xcb_rectangle_t));
            for (i = 0; i < pDirtyRegion->rdh.nCount; i++)
            {
                RECT rc;
                memcpy(&rc, pDirtyRegion->Buffer + i * sizeof(RECT), sizeof(RECT));
                rect_update.x = rc.left;
                rect_update.y = rc.top;
                rect_update.width = rc.right - rc.left;
                rect_update.height = rc.bottom - rc.top;
                memcpy(rect_updates + i * sizeof(xcb_rectangle_t), &rect_update, sizeof(xcb_rectangle_t));
            }
            xcb_xfixes_create_region(present_priv->xcb_connection, update, pDirtyRegion->rdh.nCount, rect_updates);
            free(rect_updates);
        } else
            xcb_xfixes_create_region(present_priv->xcb_connection, update, 1, &rect_update);
    }
    if (pPresentationParameters->SwapEffect == D3DSWAPEFFECT_COPY)
        options |= XCB_PRESENT_OPTION_COPY;
    cookie = xcb_present_pixmap_checked(present_priv->xcb_connection,
                                        window,
                                        present_pixmap_priv->pixmap,
                                        serial, valid, update, x_off,
                                        y_off, None, None, None, options,
                                        target_msc, 0, 0, 0, NULL);
    error = xcb_request_check(present_priv->xcb_connection, cookie); /* performs a flush */
    if (error) {
        Window root_return;
        int x_pixmap, x_window, y_pixmap, y_window;
        unsigned int width_pixmap, width_window, height_pixmap, height_window;
        unsigned int border_width_return;
        unsigned int depth_pixmap, depth_window;
        int i;
        XGetGeometry(dpy, present_pixmap_priv->pixmap, &root_return, &x_pixmap, &y_pixmap, &width_pixmap,
                     &height_pixmap, &border_width_return, &depth_pixmap);
        XGetGeometry(dpy, window, &root_return, &x_window, &y_window, &width_window,
                     &height_window, &border_width_return, &depth_window);
        ERR("Error using PRESENT. Here some debug info\n");
        ERR("Pixmap: width=%d, height=%d, depth=%d, x=%d, y=%d\n", width_pixmap, height_pixmap, depth_pixmap, x_pixmap, y_pixmap);
        ERR("Window: width=%d, height=%d, depth=%d, x=%d, y=%d\n", width_window, height_window, depth_window, x_window, y_window);
        ERR("Present parameter: PresentationInterval=%d, BackBufferCount=%d, Pending presentations=%d\n",
            pPresentationParameters->PresentationInterval,
            pPresentationParameters->BackBufferCount,
            present_priv->pixmap_present_pending
           );
        if (depth_pixmap != depth_window)
            ERR("Depths are different. PRESENT needs the pixmap and the window have same depth\n");
        pthread_mutex_unlock(&present_priv->mutex_present);
        return FALSE;
    }
    present_priv->last_target = target_msc;
    present_priv->last_serial = serial;
    present_priv->pixmap_present_pending++;
    present_pixmap_priv->present_complete_pending = TRUE;
    present_pixmap_priv->present_pending_serial = serial;
    present_pixmap_priv->released = FALSE;
    pthread_mutex_unlock(&present_priv->mutex_present);
    return TRUE;
}

BOOL
PRESENTWaitPixmapReleased(PRESENTPixmapPriv *present_pixmap_priv)
{
    PRESENTpriv *present_priv = present_pixmap_priv->present_priv;

    pthread_mutex_lock(&present_priv->mutex_present);

    PRESENTflush_events(present_priv, FALSE);

    while (!present_pixmap_priv->released) {
        /* Note: following if should not happen because we'll never
         * use two PRESENTWaitPixmapReleased in parallels on same window.
         * However it would make it work in that case */
        if (present_priv->xcb_wait) { /* we allow only one thread to dispatch events */
            pthread_mutex_lock(&present_priv->mutex_xcb_wait);
            /* here the other thread got an event but hasn't treated it yet */
            pthread_mutex_unlock(&present_priv->mutex_xcb_wait);
            pthread_mutex_unlock(&present_priv->mutex_present);
            Sleep(10); /* Let it treat the event */
            pthread_mutex_lock(&present_priv->mutex_present);
        } else if (!PRESENTwait_events(present_priv, TRUE)) {
            pthread_mutex_unlock(&present_priv->mutex_present);
            return FALSE;
        }
    }
    pthread_mutex_unlock(&present_priv->mutex_present);
    return TRUE;
}

#endif /* defined(SONAME_LIBXEXT) && defined(SONAME_LIBXFIXES) */
