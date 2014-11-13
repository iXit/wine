/*
 * X11DRV ID3DAdapter9 support functions
 *
 * Copyright 2013 Joakim Sindholt
 *                Christoph Bumiller
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
#include "wine/port.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3dadapter);

#if defined(SONAME_LIBXEXT) && \
    defined(SONAME_LIBXFIXES) && \
    defined(SONAME_LIBD3DADAPTER9)

#include "wine/d3dadapter.h"
#include "wine/library.h"
#include "wine/unicode.h"

#include "x11drv.h"

#include <d3dadapter/drm.h>

#include "xfixes.h"
#include "dri3.h"

#include <libdrm/drm.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>

#ifndef D3DPRESENT_DONOTWAIT
#define D3DPRESENT_DONOTWAIT      0x00000001
#endif

#define WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MAJOR 1
#define WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR 0

static const struct D3DAdapter9DRM *d3d9_drm = NULL;

static XContext d3d_hwnd_context;
static CRITICAL_SECTION context_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &context_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": context_section") }
};
static CRITICAL_SECTION context_section = { &critsect_debug, -1, 0, 0, 0, 0 };

const GUID IID_ID3DPresent = { 0x77D60E80, 0xF1E6, 0x11DF, { 0x9E, 0x39, 0x95, 0x0C, 0xDF, 0xD7, 0x20, 0x85 } };
const GUID IID_ID3DPresentGroup = { 0xB9C3016E, 0xF32A, 0x11DF, { 0x9C, 0x18, 0x92, 0xEA, 0xDE, 0xD7, 0x20, 0x85 } };

struct d3d_drawable
{
    Drawable drawable; /* X11 drawable */
    RECT dc_rect; /* rect relative to the X11 drawable */
    HDC hdc;
    HWND wnd; /* HWND (for convenience) */
};

struct DRI3Present
{
    /* COM vtable */
    void *vtable;
    /* IUnknown reference count */
    LONG refs;

    D3DPRESENT_PARAMETERS params;
    HWND focus_wnd;
    PRESENTpriv *present_priv;

    WCHAR devname[32];
    HCURSOR hCursor;

    DEVMODEW initial_mode;
};

struct D3DWindowBuffer
{
    Pixmap pixmap;
    PRESENTPixmapPriv *present_pixmap_priv;
};

static void
free_d3dadapter_drawable(struct d3d_drawable *d3d)
{
    ReleaseDC(d3d->wnd, d3d->hdc);
    HeapFree(GetProcessHeap(), 0, d3d);
}

void
destroy_d3dadapter_drawable(HWND hwnd)
{
    struct d3d_drawable *d3d;

    EnterCriticalSection(&context_section);
    if (!XFindContext(gdi_display, (XID)hwnd,
                      d3d_hwnd_context, (char **)&d3d)) {
        XDeleteContext(gdi_display, (XID)hwnd, d3d_hwnd_context);
        free_d3dadapter_drawable(d3d);
    }
    LeaveCriticalSection(&context_section);
}

static struct d3d_drawable *
create_d3dadapter_drawable(HWND hwnd)
{
    struct x11drv_escape_get_drawable extesc = { X11DRV_GET_DRAWABLE };
    struct d3d_drawable *d3d;

    d3d = HeapAlloc(GetProcessHeap(), 0, sizeof(*d3d));
    if (!d3d) {
        ERR("Couldn't allocate d3d_drawable.\n");
        return NULL;
    }

    d3d->hdc = GetDCEx(hwnd, 0, DCX_CACHE | DCX_CLIPSIBLINGS);
    if (ExtEscape(d3d->hdc, X11DRV_ESCAPE, sizeof(extesc), (LPCSTR)&extesc,
                  sizeof(extesc), (LPSTR)&extesc) <= 0) {
        ERR("Unexpected error in X Drawable lookup (hwnd=%p, hdc=%p)\n",
            hwnd, d3d->hdc);
        ReleaseDC(hwnd, d3d->hdc);
        HeapFree(GetProcessHeap(), 0, d3d);
        return NULL;
    }

    d3d->drawable = extesc.drawable;
    d3d->wnd = hwnd;
    d3d->dc_rect = extesc.dc_rect;

    return d3d;
}

static struct d3d_drawable *
get_d3d_drawable(HWND hwnd)
{
    struct d3d_drawable *d3d, *race;

    EnterCriticalSection(&context_section);
    if (!XFindContext(gdi_display, (XID)hwnd,
                      d3d_hwnd_context, (char **)&d3d)) {
        struct x11drv_escape_get_drawable extesc = { X11DRV_GET_DRAWABLE };

        /* check if the window has moved since last we used it */
        if (ExtEscape(d3d->hdc, X11DRV_ESCAPE, sizeof(extesc), (LPCSTR)&extesc,
                      sizeof(extesc), (LPSTR)&extesc) <= 0) {
            WARN("Window update check failed (hwnd=%p, hdc=%p)\n",
                 hwnd, d3d->hdc);
        }

        if (!EqualRect(&d3d->dc_rect, &extesc.dc_rect))
            d3d->dc_rect = extesc.dc_rect;

        return d3d;
    }
    LeaveCriticalSection(&context_section);

    TRACE("No d3d_drawable attached to hwnd %p, creating one.\n", hwnd);

    d3d = create_d3dadapter_drawable(hwnd);
    if (!d3d) { return NULL; }

    EnterCriticalSection(&context_section);
    if (!XFindContext(gdi_display, (XID)hwnd,
                      d3d_hwnd_context, (char **)&race)) {
        /* apparently someone beat us to creating this d3d drawable. Let's not
           waste more time with X11 calls and just use theirs instead. */
        free_d3dadapter_drawable(d3d);
        return race;
    }
    XSaveContext(gdi_display, (XID)hwnd, d3d_hwnd_context, (char *)d3d);
    return d3d;
}

static void
release_d3d_drawable(struct d3d_drawable *d3d)
{
    if (d3d) { LeaveCriticalSection(&context_section); }
}

static ULONG WINAPI
DRI3Present_AddRef( struct DRI3Present *This )
{
    ULONG refs = InterlockedIncrement(&This->refs);
    TRACE("%p increasing refcount to %u.\n", This, refs);
    return refs;
}

static ULONG WINAPI
DRI3Present_Release( struct DRI3Present *This )
{
    ULONG refs = InterlockedDecrement(&This->refs);
    TRACE("%p decreasing refcount to %u.\n", This, refs);
    if (refs == 0) {
        /* dtor */
        ChangeDisplaySettingsExW(This->devname, &(This->initial_mode), 0, CDS_FULLSCREEN, NULL);
        PRESENTDestroy(gdi_display, This->present_priv);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return refs;
}

static HRESULT WINAPI
DRI3Present_QueryInterface( struct DRI3Present *This,
                            REFIID riid,
                            void **ppvObject )
{
    if (!ppvObject) { return E_POINTER; }

    if (IsEqualGUID(&IID_ID3DPresent, riid) ||
        IsEqualGUID(&IID_IUnknown, riid)) {
        *ppvObject = This;
        DRI3Present_AddRef(This);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *ppvObject = NULL;

    return E_NOINTERFACE;
}

static void
DRI3Present_ChangePresentParameters( struct DRI3Present *This,
                                    D3DPRESENT_PARAMETERS *params,
                                    BOOL first_time);

static HRESULT WINAPI
DRI3Present_SetPresentParameters( struct DRI3Present *This,
                                  D3DPRESENT_PARAMETERS *pPresentationParameters,
                                  D3DDISPLAYMODEEX *pFullscreenDisplayMode )
{
    if (pFullscreenDisplayMode)
        WARN("Ignoring pFullscreenDisplayMode\n");
    DRI3Present_ChangePresentParameters(This, pPresentationParameters, FALSE);
    return D3D_OK;
}

static HRESULT WINAPI
DRI3Present_D3DWindowBufferFromDmaBuf( struct DRI3Present *This,
                       int dmaBufFd,
                       int width,
                       int height,
                       int stride,
                       int depth,
                       int bpp,
                       struct D3DWindowBuffer **out)
{
    Pixmap pixmap;

    if (!DRI3PixmapFromDmaBuf(gdi_display, DefaultScreen(gdi_display),
                              dmaBufFd, width, height, stride, depth,
                              bpp, &pixmap ))
        return D3DERR_DRIVERINTERNALERROR;

    *out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                    sizeof(struct D3DWindowBuffer));
    (*out)->pixmap = pixmap;
    PRESENTPixmapInit(This->present_priv, pixmap, &((*out)->present_pixmap_priv));
    return D3D_OK;
}

static HRESULT WINAPI
DRI3Present_DestroyD3DWindowBuffer( struct DRI3Present *This,
                           struct D3DWindowBuffer *buffer )
{
    /* the pixmap is managed by the PRESENT backend.
     * But if it can delete it right away, we may have
     * better performance */
    PRESENTTryFreePixmap(buffer->present_pixmap_priv);
    HeapFree(GetProcessHeap(), 0, buffer);
    return D3D_OK;
}

static HRESULT WINAPI
DRI3Present_WaitBufferReleased( struct DRI3Present *This,
                                struct D3DWindowBuffer *buffer)
{
    PRESENTWaitPixmapReleased(buffer->present_pixmap_priv);
    return D3D_OK;
}

static HRESULT WINAPI
DRI3Present_FrontBufferCopy( struct DRI3Present *This,
                             struct D3DWindowBuffer *buffer )
{
    /* TODO: use dc_rect */
    if (PRESENTHelperCopyFront(gdi_display, buffer->present_pixmap_priv))
        return D3D_OK;
    else
        return D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI
DRI3Present_PresentBuffer( struct DRI3Present *This,
                           struct D3DWindowBuffer *buffer,
                           HWND hWndOverride,
                           const RECT *pSourceRect,
                           const RECT *pDestRect,
                           const RGNDATA *pDirtyRegion,
                           DWORD Flags )
{
    struct d3d_drawable *d3d;
    RECT dest_translate;

    if (hWndOverride) {
        d3d = get_d3d_drawable(hWndOverride);
    } else if (This->params.hDeviceWindow) {
        d3d = get_d3d_drawable(This->params.hDeviceWindow);
    } else {
        d3d = get_d3d_drawable(This->focus_wnd);
    }
    if (!d3d) { return D3DERR_DRIVERINTERNALERROR; }

    if (d3d->dc_rect.top != 0 &&
        d3d->dc_rect.left != 0) {
        if (!pDestRect)
            pDestRect = (const RECT *) &(d3d->dc_rect);
        else {
            dest_translate.top = pDestRect->top + d3d->dc_rect.top;
            dest_translate.left = pDestRect->left + d3d->dc_rect.left;
            dest_translate.bottom = pDestRect->bottom + d3d->dc_rect.bottom;
            dest_translate.right = pDestRect->right + d3d->dc_rect.right;
            pDestRect = (const RECT *) &dest_translate;
        }
    }

    if (!PRESENTPixmap(gdi_display, d3d->drawable, buffer->present_pixmap_priv,
                       &This->params, pSourceRect, pDestRect, pDirtyRegion))
        return D3DERR_DRIVERINTERNALERROR;

    release_d3d_drawable(d3d);

    return D3D_OK;
}

static HRESULT WINAPI
DRI3Present_GetRasterStatus( struct DRI3Present *This,
                             D3DRASTER_STATUS *pRasterStatus )
{
    FIXME("(%p, %p), stub!\n", This, pRasterStatus);
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI
DRI3Present_GetDisplayMode( struct DRI3Present *This,
                            D3DDISPLAYMODEEX *pMode,
                            D3DDISPLAYROTATION *pRotation )
{
    DEVMODEW dm;

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);

    EnumDisplaySettingsExW(This->devname, ENUM_CURRENT_SETTINGS, &dm, 0);
    pMode->Width = dm.dmPelsWidth;
    pMode->Height = dm.dmPelsHeight;
    pMode->RefreshRate = dm.dmDisplayFrequency;
    pMode->ScanLineOrdering = (dm.dmDisplayFlags & DM_INTERLACED) ?
                                  D3DSCANLINEORDERING_INTERLACED :
                                  D3DSCANLINEORDERING_PROGRESSIVE;

    /* XXX This is called "guessing" */
    switch (dm.dmBitsPerPel) {
        case 32: pMode->Format = D3DFMT_X8R8G8B8; break;
        case 24: pMode->Format = D3DFMT_R8G8B8; break;
        case 16: pMode->Format = D3DFMT_R5G6B5; break;
        default:
            WARN("Unknown display format with %u bpp.\n", dm.dmBitsPerPel);
            pMode->Format = D3DFMT_UNKNOWN;
    }

    switch (dm.dmDisplayOrientation) {
        case DMDO_DEFAULT: *pRotation = D3DDISPLAYROTATION_IDENTITY; break;
        case DMDO_90:      *pRotation = D3DDISPLAYROTATION_90; break;
        case DMDO_180:     *pRotation = D3DDISPLAYROTATION_180; break;
        case DMDO_270:     *pRotation = D3DDISPLAYROTATION_270; break;
        default:
            WARN("Unknown display rotation %u.\n", dm.dmDisplayOrientation);
            *pRotation = D3DDISPLAYROTATION_IDENTITY;
    }

    return D3D_OK;
}

static HRESULT WINAPI
DRI3Present_GetPresentStats( struct DRI3Present *This,
                             D3DPRESENTSTATS *pStats )
{
    FIXME("(%p, %p), stub!\n", This, pStats);
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI
DRI3Present_GetCursorPos( struct DRI3Present *This,
                          POINT *pPoint )
{
    BOOL ok;
    if (!pPoint)
        return D3DERR_INVALIDCALL;
    ok = GetCursorPos(pPoint);
    ok = ok && ScreenToClient(This->focus_wnd, pPoint);
    return ok ? S_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI
DRI3Present_SetCursorPos( struct DRI3Present *This,
                          POINT *pPoint )
{
    if (!pPoint)
        return D3DERR_INVALIDCALL;
    return SetCursorPos(pPoint->x, pPoint->y);
}

static HRESULT WINAPI
DRI3Present_SetCursor( struct DRI3Present *This,
                       void *pBitmap,
                       POINT *pHotspot,
                       BOOL bShow )
{
   if (pBitmap) {
      ICONINFO info;
      HCURSOR cursor;

      DWORD mask[32];
      memset(mask, ~0, sizeof(mask));

      if (!pHotspot)
         return D3DERR_INVALIDCALL;
      info.fIcon = FALSE;
      info.xHotspot = pHotspot->x;
      info.yHotspot = pHotspot->y;
      info.hbmMask = CreateBitmap(32, 32, 1, 1, mask);
      info.hbmColor = CreateBitmap(32, 32, 1, 32, pBitmap);

      cursor = CreateIconIndirect(&info);
      if (info.hbmMask) DeleteObject(info.hbmMask);
      if (info.hbmColor) DeleteObject(info.hbmColor);
      if (cursor)
         DestroyCursor(This->hCursor);
      This->hCursor = cursor;
   }
   SetCursor(bShow ? This->hCursor : NULL);

   return D3D_OK;
}

static HRESULT WINAPI
DRI3Present_SetGammaRamp( struct DRI3Present *This,
                          const D3DGAMMARAMP *pRamp,
                          HWND hWndOverride )
{
    HWND hWnd = hWndOverride ? hWndOverride : This->focus_wnd;
    HDC hdc;
    BOOL ok;
    if (!pRamp) {
        return D3DERR_INVALIDCALL;
    }
    hdc = GetDC(hWnd);
    ok = SetDeviceGammaRamp(hdc, (void *)pRamp);
    ReleaseDC(hWnd, hdc);
    return ok ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI
DRI3Present_GetWindowInfo( struct DRI3Present *This,
                           HWND hWnd,
                           int *width, int *height, int *depth )
{
    HRESULT hr;
    RECT pRect;

    if (!hWnd)
        hWnd = This->focus_wnd;
    hr = GetClientRect(hWnd, &pRect);
    if (!hr)
        return D3DERR_INVALIDCALL;
    *width = pRect.right - pRect.left;
    *height = pRect.bottom - pRect.top;
    *depth = 24; //TODO
    return D3D_OK;
}

static LONG fullscreen_style(LONG style)
{
    /* Make sure the window is managed, otherwise we won't get keyboard input. */
    style |= WS_POPUP | WS_SYSMENU;
    style &= ~(WS_CAPTION | WS_THICKFRAME);

    return style;
}

static LONG fullscreen_exstyle(LONG exstyle)
{
    /* Filter out window decorations. */
    exstyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);

    return exstyle;
}


/*----------*/


static ID3DPresentVtbl DRI3Present_vtable = {
    (void *)DRI3Present_QueryInterface,
    (void *)DRI3Present_AddRef,
    (void *)DRI3Present_Release,
    (void *)DRI3Present_SetPresentParameters,
    (void *)DRI3Present_D3DWindowBufferFromDmaBuf,
    (void *)DRI3Present_DestroyD3DWindowBuffer,
    (void *)DRI3Present_WaitBufferReleased,
    (void *)DRI3Present_FrontBufferCopy,
    (void *)DRI3Present_PresentBuffer,
    (void *)DRI3Present_GetRasterStatus,
    (void *)DRI3Present_GetDisplayMode,
    (void *)DRI3Present_GetPresentStats,
    (void *)DRI3Present_GetCursorPos,
    (void *)DRI3Present_SetCursorPos,
    (void *)DRI3Present_SetCursor,
    (void *)DRI3Present_SetGammaRamp,
    (void *)DRI3Present_GetWindowInfo
};

static void
DRI3Present_ChangePresentParameters( struct DRI3Present *This,
                                    D3DPRESENT_PARAMETERS *params,
                                    BOOL first_time)
{
    HWND draw_window;
    RECT rect;

    (void) first_time; /* will be used to manage screen res if windowed mode change */
    /* TODO: don't do anything if nothing changed */
    /* sanitize presentation parameters */
    draw_window = params->hDeviceWindow ? params->hDeviceWindow : This->focus_wnd;

    if (!GetClientRect(draw_window, &rect)) {
        WARN("GetClientRect failed.\n");
        rect.right = 640;
        rect.bottom = 480;
    }

    if (params->BackBufferWidth == 0) {
        params->BackBufferWidth = rect.right - rect.left;
    }
    if (params->BackBufferHeight == 0) {
        params->BackBufferHeight = rect.bottom - rect.top;
    }

    if (!params->Windowed) {
        /* TODO Store initial config and restore it when leaving fullscreen, or when leaving wine*/
        LONG style, exstyle;
        DEVMODEW newMode;

        newMode.dmPelsWidth = params->BackBufferWidth;
        newMode.dmPelsHeight = params->BackBufferHeight;
        newMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        ChangeDisplaySettingsExW(This->devname, &newMode, 0, CDS_FULLSCREEN, NULL);

        style = fullscreen_style(0);
        exstyle = fullscreen_exstyle(0);

        SetWindowLongW(draw_window, GWL_STYLE, style);
        SetWindowLongW(draw_window, GWL_EXSTYLE, exstyle);
        SetWindowPos(draw_window, HWND_TOPMOST, 0, 0, params->BackBufferWidth, params->BackBufferHeight, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    } else if (!first_time && !This->params.Windowed)
        ChangeDisplaySettingsExW(This->devname, &(This->initial_mode), 0, CDS_FULLSCREEN, NULL);

    This->params = *params;
}

static HRESULT
DRI3Present_new( Display *dpy,
                 const WCHAR *devname,
                 D3DPRESENT_PARAMETERS *params,
                 HWND focus_wnd,
                 struct DRI3Present **out )
{
    struct DRI3Present *This;

    if (!focus_wnd) { focus_wnd = params->hDeviceWindow; }
    if (!focus_wnd) {
        ERR("No focus HWND specified for presentation backend.\n");
        return D3DERR_INVALIDCALL;
    }

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                     sizeof(struct DRI3Present));
    if (!This) {
        ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }

    This->vtable = &DRI3Present_vtable;
    This->refs = 1;
    This->focus_wnd = focus_wnd;

    strcpyW(This->devname, devname);

    ZeroMemory(&(This->initial_mode), sizeof(This->initial_mode));
    This->initial_mode.dmSize = sizeof(This->initial_mode);

    EnumDisplaySettingsExW(This->devname, ENUM_CURRENT_SETTINGS, &(This->initial_mode), 0);

    DRI3Present_ChangePresentParameters(This, params, TRUE);

    PRESENTInit(dpy, &(This->present_priv));

    *out = This;

    return D3D_OK;
}

struct DRI3PresentGroup
{
    /* COM vtable */
    void *vtable;
    /* IUnknown reference count */
    LONG refs;

    struct DRI3Present **present_backends;
    unsigned npresent_backends;
};

static ULONG WINAPI
DRI3PresentGroup_AddRef( struct DRI3PresentGroup *This )
{
    ULONG refs = InterlockedIncrement(&This->refs);
    TRACE("%p increasing refcount to %u.\n", This, refs);
    return refs;
}

static ULONG WINAPI
DRI3PresentGroup_Release( struct DRI3PresentGroup *This )
{
    ULONG refs = InterlockedDecrement(&This->refs);
    TRACE("%p decreasing refcount to %u.\n", This, refs);
    if (refs == 0) {
        unsigned i;
        if (This->present_backends) {
            for (i = 0; i < This->npresent_backends; ++i) {
                DRI3Present_Release(This->present_backends[i]);
            }
            HeapFree(GetProcessHeap(), 0, This->present_backends);
        }
        HeapFree(GetProcessHeap(), 0, This);
    }
    return refs;
}

static HRESULT WINAPI
DRI3PresentGroup_QueryInterface( struct DRI3PresentGroup *This,
                                 REFIID riid,
                                 void **ppvObject )
{
    if (!ppvObject) { return E_POINTER; }
    if (IsEqualGUID(&IID_ID3DPresentGroup, riid) ||
        IsEqualGUID(&IID_IUnknown, riid)) {
        *ppvObject = This;
        DRI3PresentGroup_AddRef(This);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *ppvObject = NULL;

    return E_NOINTERFACE;
}

static UINT WINAPI
DRI3PresentGroup_GetMultiheadCount( struct DRI3PresentGroup *This )
{
    FIXME("(%p), stub!\n", This);
    return 1;
}

static HRESULT WINAPI
DRI3PresentGroup_GetPresent( struct DRI3PresentGroup *This,
                             UINT Index,
                             ID3DPresent **ppPresent )
{
    if (Index >= DRI3PresentGroup_GetMultiheadCount(This)) {
        ERR("Index >= MultiHeadCount\n");
        return D3DERR_INVALIDCALL;
    }
    DRI3Present_AddRef(This->present_backends[Index]);
    *ppPresent = (ID3DPresent *)This->present_backends[Index];

    return D3D_OK;
}

static HRESULT WINAPI
DRI3PresentGroup_CreateAdditionalPresent( struct DRI3PresentGroup *This,
                                          D3DPRESENT_PARAMETERS *pPresentationParameters,
                                          ID3DPresent **ppPresent )
{
    FIXME("(%p, %p, %p), stub!\n", This, pPresentationParameters, ppPresent);
    return D3DERR_INVALIDCALL;
}

static void WINAPI
DRI3PresentGroup_GetVersion( struct DRI3PresentGroup *This,
                             int *major,
                             int *minor)
{
    *major = WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MAJOR;
    *minor = WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR;
}

static ID3DPresentGroupVtbl DRI3PresentGroup_vtable = {
    (void *)DRI3PresentGroup_QueryInterface,
    (void *)DRI3PresentGroup_AddRef,
    (void *)DRI3PresentGroup_Release,
    (void *)DRI3PresentGroup_GetMultiheadCount,
    (void *)DRI3PresentGroup_GetPresent,
    (void *)DRI3PresentGroup_CreateAdditionalPresent,
    (void *)DRI3PresentGroup_GetVersion
};

static HRESULT
dri3_create_present_group( const WCHAR *device_name,
                           UINT adapter,
                           HWND focus_wnd,
                           D3DPRESENT_PARAMETERS *params,
                           unsigned nparams,
                           ID3DPresentGroup **group )
{
    struct DRI3PresentGroup *This =
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                  sizeof(struct DRI3PresentGroup));
    DISPLAY_DEVICEW dd;
    HRESULT hr;
    unsigned i;

    if (!This) {
        ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }

    This->vtable = &DRI3PresentGroup_vtable;
    This->refs = 1;
    This->npresent_backends = nparams;
    This->present_backends = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       This->npresent_backends *
                                       sizeof(struct DRI3Present *));
    if (!This->present_backends) {
        DRI3PresentGroup_Release(This);
        ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }

    if (nparams != 1) { adapter = 0; }
    for (i = 0; i < This->npresent_backends; ++i) {
        /* find final device name */
        if (!EnumDisplayDevicesW(device_name, adapter+i, &dd, 0)) {
            WARN("Couldn't find subdevice %d from `%s'\n",
                 i, debugstr_w(device_name));
        }

        /* create an ID3DPresent for it */
        hr = DRI3Present_new(gdi_display, dd.DeviceName, &params[i],
                             focus_wnd, &This->present_backends[i]);
        if (FAILED(hr)) {
            DRI3PresentGroup_Release(This);
            return hr;
        }
    }

    *group = (ID3DPresentGroup *)This;
    TRACE("Returning %p\n", *group);

    return D3D_OK;
}

static HRESULT
dri3_create_adapter9( HDC hdc,
                      ID3DAdapter9 **out )
{
    struct x11drv_escape_get_drawable extesc = { X11DRV_GET_DRAWABLE };
    HRESULT hr;
    int fd;

    if (!d3d9_drm) {
        WARN("DRM drivers are not supported on your system.\n");
        return D3DERR_DRIVERINTERNALERROR;
    }

    if (ExtEscape(hdc, X11DRV_ESCAPE, sizeof(extesc), (LPCSTR)&extesc,
                  sizeof(extesc), (LPSTR)&extesc) <= 0) {
        WARN("X11 drawable lookup failed (hdc=%p)\n", hdc);
    }

    if (!DRI3Open(gdi_display, DefaultScreen(gdi_display), &fd)) {
        WARN("DRI3Open failed (fd=%d)\n", fd);
        return D3DERR_DRIVERINTERNALERROR;
    }

    hr = d3d9_drm->create_adapter(fd, out);
    if (FAILED(hr)) {
        WARN("Unable to create ID3DAdapter9 (fd=%d)\n", fd);
        return hr;
    }

    TRACE("Created ID3DAdapter9 with fd %d\n", fd);

    return D3D_OK;
}

static BOOL
has_d3dadapter( void )
{
    static const void * WINAPI (*pD3DAdapter9GetProc)(const char *);
    static void *handle = NULL;
    static int done = 0;

    int xfmaj, xfmin;
    char errbuf[256];

    /* like in opengl.c (single threaded assumption OK?) */
    if (done) { return handle != NULL; }
    done = 1;

    /*  */
    if (!usexfixes) {
        ERR("%s needs Xfixes.\n", SONAME_LIBD3DADAPTER9);
        return FALSE;
    }

    handle = wine_dlopen(SONAME_LIBD3DADAPTER9, RTLD_GLOBAL|RTLD_NOW,
                         errbuf, sizeof(errbuf));
    if (!handle) {
        ERR("Failed to load %s: %s\n", SONAME_LIBD3DADAPTER9, errbuf);
        goto cleanup;
    }

    /* find our entry point in libd3dadapter9 */
    pD3DAdapter9GetProc = wine_dlsym(handle, "D3DAdapter9GetProc",
                                     errbuf, sizeof(errbuf));
    if (!pD3DAdapter9GetProc) {
        ERR("Failed to get the entry point from %s: %s",
            SONAME_LIBD3DADAPTER9, errbuf);
        goto cleanup;
    }

    /* get a handle to the drm backend struct */
    d3d9_drm = pD3DAdapter9GetProc(D3DADAPTER9DRM_NAME);
    if (!d3d9_drm) {
        ERR("%s doesn't support the `%s' backend.\n",
            SONAME_LIBD3DADAPTER9, D3DADAPTER9DRM_NAME);
        goto cleanup;
    }

    /* verify that we're binary compatible */
    if (d3d9_drm->major_version != D3DADAPTER9DRM_MAJOR) {
        ERR("Version mismatch. %s has %d.%d, was expecting %d.x\n",
            SONAME_LIBD3DADAPTER9, d3d9_drm->major_version,
            d3d9_drm->minor_version, D3DADAPTER9DRM_MAJOR);
        goto cleanup;
    }

    /* this will be used to store d3d_drawables */
    d3d_hwnd_context = XUniqueContext();

    if (!DRI3CheckExtension(gdi_display, 1, 0) ||
        !PRESENTCheckExtension(gdi_display, 1, 0)) {
        ERR("Unable to query DRI3 or PRESENT\n");
        goto cleanup;
    }

    /* query XFixes */
    if (!pXFixesQueryVersion(gdi_display, &xfmaj, &xfmin)) {
        ERR("Unable to query XFixes extension.\n");
        return D3DERR_DRIVERINTERNALERROR;
    }
    TRACE("Got XFixes version %u.%u\n", xfmaj, xfmin);
    return TRUE;

cleanup:
    ERR("\033[1;31m\nNative Direct3D 9 will be unavailable."
        "\nFor more information visit https://wiki.ixit.cz/d3d9\033[0m\n");
    if (handle) {
        wine_dlclose(handle, NULL, 0);
        handle = NULL;
    }

    return FALSE;
}

static struct d3dadapter_funcs dri3_driver = {
    dri3_create_present_group,          /* create_present_group */
    dri3_create_adapter9,               /* create_adapter9 */
};

struct d3dadapter_funcs *
get_d3d_dri3_driver(UINT version)
{
    if (version != WINE_D3DADAPTER_DRIVER_VERSION) {
        ERR("Version mismatch. d3d* wants %u but winex11 has "
            "version %u\n", version, WINE_D3DADAPTER_DRIVER_VERSION);
        return NULL;
    }
    if (has_d3dadapter()) { return &dri3_driver; }
    return NULL;
}

#else /* defined(SONAME_LIBXEXT) && \
         defined(SONAME_LIBXFIXES) && \
         defined(SONAME_LIBD3DADAPTER9) */

struct d3dadapter_funcs;

void
destroy_d3dadapter_drawable(HWND hwnd)
{
}

static BOOL
has_d3dadapter( void )
{
    FIXME("\033[0;31m\nWine source code has been compiled without native Direct3D 9 support."
          "\nFor more information visit https://wiki.ixit.cz/d3d9\033[0m\n");
    return FALSE;
}

struct d3dadapter_funcs *
get_d3d_dri3_driver(UINT version)
{
    return NULL;
}

#endif /* defined(SONAME_LIBXEXT) && \
          defined(SONAME_LIBXFIXES) && \
          defined(SONAME_LIBD3DADAPTER9) */
