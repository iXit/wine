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

#include <X11/Xlib.h>
#include "xfixes.h"
#include "dri2.h"

#include <libdrm/drm.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>

struct d3dadapter_info {
    unsigned dri2_major, dri2_minor;
};

static const struct D3DAdapter9DRM *d3d9_drm = NULL;
static struct d3dadapter_info d3d_info;

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
    HDC hdc;
    HWND wnd; /* HWND (for convenience) */
    RECT dc_rect; /* rect relative to the X11 drawable */
    RECT dest_rect; /* dest rect used when creating the X11 region */
    XserverRegion region; /* X11 region matching dc_rect */
};

struct DRI2Present
{
    /* COM vtable */
    void *vtable;
    /* IUnknown reference count */
    LONG refs;

    D3DPRESENT_PARAMETERS params;
    HWND focus_wnd;

    struct {
        Drawable drawable;
        XserverRegion region;
    } cache;

    WCHAR devname[32];
    HCURSOR hCursor;
};

static void
free_d3dadapter_drawable(struct d3d_drawable *d3d)
{
    DRI2DestroyDrawable(gdi_display, d3d->drawable);
    ReleaseDC(d3d->wnd, d3d->hdc);
    if (d3d->region) { pXFixesDestroyRegion(gdi_display, d3d->region); }
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
    SetRect(&d3d->dest_rect, 0, 0, 0, 0);
    d3d->region = 0; /* because of pDestRect, this is set later */

    /*if (!DRI2CreateDrawable(gdi_display, d3d->drawable)) {
        ERR("DRI2CreateDrawable failed (hwnd=%p, drawable=%u).\n",
            hwnd, d3d->drawable);
        HeapFree(GetProcessHeap(), 0, d3d);
        return NULL;
    }*/
    DRI2CreateDrawable(gdi_display, d3d->drawable);

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

        /* update the data and destroy the cached (now invalid) region */
        if (!EqualRect(&d3d->dc_rect, &extesc.dc_rect)) {
            d3d->dc_rect = extesc.dc_rect;
            if (d3d->region) {
                pXFixesDestroyRegion(gdi_display, d3d->region);
                d3d->region = 0;
            }
        }

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
DRI2Present_AddRef( struct DRI2Present *This )
{
    ULONG refs = InterlockedIncrement(&This->refs);
    TRACE("%p increasing refcount to %u.\n", This, refs);
    return refs;
}

static ULONG WINAPI
DRI2Present_Release( struct DRI2Present *This )
{
    ULONG refs = InterlockedDecrement(&This->refs);
    TRACE("%p decreasing refcount to %u.\n", This, refs);
    if (refs == 0) {
        /* dtor */
        HeapFree(GetProcessHeap(), 0, This);
    }
    return refs;
}

static HRESULT WINAPI
DRI2Present_QueryInterface( struct DRI2Present *This,
                            REFIID riid,
                            void **ppvObject )
{
    if (!ppvObject) { return E_POINTER; }

    if (IsEqualGUID(&IID_ID3DPresent, riid) ||
        IsEqualGUID(&IID_IUnknown, riid)) {
        *ppvObject = This;
        DRI2Present_AddRef(This);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *ppvObject = NULL;

    return E_NOINTERFACE;
}

static HRESULT WINAPI
DRI2Present_GetPresentParameters( struct DRI2Present *This,
                                  D3DPRESENT_PARAMETERS *pPresentationParameters )
{
    *pPresentationParameters = This->params;
    return D3D_OK;
}

static HRESULT WINAPI
DRI2Present_GetBuffer( struct DRI2Present *This,
                              HWND hWndOverride,
                              void *pBuffer,
                              const RECT *pDestRect,
                              RECT *pRect,
                              RGNDATA **ppRegion )
{
    static const unsigned attachments[] = { DRI2BufferBackLeft };

    struct d3d_drawable *d3d;
    D3DDRM_BUFFER *drmbuf = pBuffer;
    DRI2Buffer *buffers;
    RECT dest;
    unsigned width, height, n;

    TRACE("(This=%p, hWndOverride=%p, pBuffer=%p, pRect=%p, ppRegion=%p)\n",
          This, hWndOverride, pBuffer, pRect, ppRegion);

    if (hWndOverride) {
        d3d = get_d3d_drawable(hWndOverride);
    } else if (This->params.hDeviceWindow) {
        d3d = get_d3d_drawable(This->params.hDeviceWindow);
    } else {
        d3d = get_d3d_drawable(This->focus_wnd);
    }
    if (!d3d) { return D3DERR_DRIVERINTERNALERROR; }

    {
        /* TODO: don't pass rgndata to the driver, but use it for DRI2CopyRegion
        DWORD rgn_size;
        HRGN hrgn = CreateRectRgn(0, 0, 0, 0);
        if (GetWindowRgn(This->current_window.real, hrgn) != _ERROR) {
            rgn_size = GetRegionData(hrgn, 0, NULL);
            This->rgndata = HeapAlloc(GetProcessHeap(), 0, rgn_size);
            GetRegionData(hrgn, rgn_size, This->rgndata);
        }
        DeleteObject(hrgn);
        if (!This->rgndata) {
            return D3DERR_DRIVERINTERNALERROR;
        }*/
        *ppRegion = NULL;
    }

    /* XXX base this on events instead of calling every single frame */
    if ((n = DRI2GetBuffers(gdi_display, d3d->drawable, attachments, 1,
                            &width, &height, &buffers)) < 1) {
        ERR("DRI2GetBuffers failed (drawable=%u, n=%u)\n",
            (unsigned)d3d->drawable, n);
        release_d3d_drawable(d3d);
        return D3DERR_DRIVERINTERNALERROR;
    }

    drmbuf->iName = buffers[0].name;
    drmbuf->dwWidth = width;
    drmbuf->dwHeight = height;
    drmbuf->dwStride = buffers[0].pitch;
    drmbuf->dwCPP = buffers[0].cpp;
    HeapFree(GetProcessHeap(), 0, buffers);

    /* return the offset region to the driver */
    pRect->left = d3d->dc_rect.left;
    pRect->top = d3d->dc_rect.top;
    pRect->right = d3d->dc_rect.right;
    pRect->bottom = d3d->dc_rect.bottom;

    TRACE("pRect=(%u..%u)x(%u..%u)\n",
          pRect->left, pRect->right, pRect->top, pRect->bottom);

    /* if pDestRect is set, calculate the final region */
    dest = *pRect;
    if (pDestRect) {
        dest.left += pDestRect->left;
        dest.top += pDestRect->top;
        dest.right = pDestRect->right + dest.left;
        dest.bottom = pDestRect->bottom + dest.top;
        if (dest.left > pRect->right) { dest.left = pRect->right; }
        if (dest.top > pRect->bottom) { dest.top = pRect->bottom; }
        if (dest.right > pRect->right) { dest.right = pRect->right; }
        if (dest.bottom > pRect->bottom) { dest.bottom = pRect->bottom; }

        TRACE("dest=(%u..%u)x(%u..%u)\n",
              dest.left, dest.right, dest.top, dest.bottom);
    }

    /* if the new dest region doesn't match the cached one, destroy it */
    if (!EqualRect(&d3d->dest_rect, &dest) && d3d->region) {
        pXFixesDestroyRegion(gdi_display, d3d->region);
        d3d->region = 0;
    }

    /* create region to match the new destination */
    if (!d3d->region) {
        XRectangle xrect;

        xrect.x = dest.left;
        xrect.y = dest.top;
        xrect.width = dest.right - dest.left;
        xrect.height = dest.bottom - dest.top;

        d3d->region = pXFixesCreateRegion(gdi_display, &xrect, 1);
        d3d->dest_rect = dest;

        TRACE("XFixes rect (x=%u, y=%u, w=%u, h=%u)\n",
              xrect.x, xrect.y, xrect.width, xrect.height);
    }

    This->cache.drawable = d3d->drawable;
    This->cache.region = d3d->region;

    release_d3d_drawable(d3d);

    return D3D_OK;
}

static HRESULT WINAPI
DRI2Present_GetFrontBuffer( struct DRI2Present *This,
                            void *pBuffer )
{
    FIXME("(%p, %p), stub!\n", This, pBuffer);
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI
DRI2Present_Present( struct DRI2Present *This,
                     DWORD Flags )
{
    TRACE("(This=%p, Flags=%x)\n", This, Flags);

    XFlush(gdi_display);

    if (1/*This->dri2_minor < 3*/) {
        if (!DRI2CopyRegion(gdi_display, This->cache.drawable,
                            This->cache.region, DRI2BufferFrontLeft,
                            DRI2BufferBackLeft)) {
            ERR("DRI2CopyRegion failed (drawable=%u, region=%u)\n",
                (unsigned)This->cache.drawable, (unsigned)This->cache.region);
            return D3DERR_DRIVERINTERNALERROR;
        }
        /* XXX if (!(Flags & D3DPRESENT_DONOTWAIT)) { */
    }

    return D3D_OK;
}

static HRESULT WINAPI
DRI2Present_GetRasterStatus( struct DRI2Present *This,
                             D3DRASTER_STATUS *pRasterStatus )
{
    FIXME("(%p, %p), stub!\n", This, pRasterStatus);
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI
DRI2Present_GetDisplayMode( struct DRI2Present *This,
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
DRI2Present_GetPresentStats( struct DRI2Present *This,
                             D3DPRESENTSTATS *pStats )
{
    FIXME("(%p, %p), stub!\n", This, pStats);
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI
DRI2Present_GetCursorPos( struct DRI2Present *This,
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
DRI2Present_SetCursorPos( struct DRI2Present *This,
                          POINT *pPoint )
{
    if (!pPoint)
        return D3DERR_INVALIDCALL;
    return SetCursorPos(pPoint->x, pPoint->y);
}

static HRESULT WINAPI
DRI2Present_SetCursor( struct DRI2Present *This,
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
DRI2Present_SetGammaRamp( struct DRI2Present *This,
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
DRI2Present_GetWindowRect( struct DRI2Present *This,
                           HWND hWnd,
                           LPRECT pRect )
{
    if (!hWnd)
        hWnd = This->focus_wnd;
    return GetClientRect(hWnd, pRect) ? D3D_OK : D3DERR_INVALIDCALL;
}

static ID3DPresentVtbl DRI2Present_vtable = {
    (void *)DRI2Present_QueryInterface,
    (void *)DRI2Present_AddRef,
    (void *)DRI2Present_Release,
    (void *)DRI2Present_GetPresentParameters,
    (void *)DRI2Present_GetBuffer,
    (void *)DRI2Present_GetFrontBuffer,
    (void *)DRI2Present_Present,
    (void *)DRI2Present_GetRasterStatus,
    (void *)DRI2Present_GetDisplayMode,
    (void *)DRI2Present_GetPresentStats,
    (void *)DRI2Present_GetCursorPos,
    (void *)DRI2Present_SetCursorPos,
    (void *)DRI2Present_SetCursor,
    (void *)DRI2Present_SetGammaRamp,
    (void *)DRI2Present_GetWindowRect
};

static HRESULT
DRI2Present_new( Display *dpy,
                 const WCHAR *devname,
                 D3DPRESENT_PARAMETERS *params,
                 HWND focus_wnd,
                 struct DRI2Present **out )
{
    struct DRI2Present *This;
    HWND draw_window;
    RECT rect;

    if (!focus_wnd) { focus_wnd = params->hDeviceWindow; }
    if (!focus_wnd) {
        ERR("No focus HWND specified for presentation backend.\n");
        return D3DERR_INVALIDCALL;
    }
    draw_window = params->hDeviceWindow ? params->hDeviceWindow : focus_wnd;

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                     sizeof(struct DRI2Present));
    if (!This) {
        ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }

    This->vtable = &DRI2Present_vtable;
    This->refs = 1;
    This->focus_wnd = focus_wnd;

    /* sanitize presentation parameters */
    if (params->SwapEffect == D3DSWAPEFFECT_COPY &&
        params->BackBufferCount > 1) {
        WARN("present: BackBufferCount > 1 when SwapEffect == "
             "D3DSWAPEFFECT_COPY.\n");
        params->BackBufferCount = 1;
    }

    /* XXX 30 for Ex */
    if (params->BackBufferCount > 3) {
        WARN("present: BackBufferCount > 3.\n");
        params->BackBufferCount = 3;
    }

    if (params->BackBufferCount == 0) {
        params->BackBufferCount = 1;
    }

    if (params->BackBufferFormat == D3DFMT_UNKNOWN) {
        params->BackBufferFormat = D3DFMT_A8R8G8B8;
    }

    if (!GetClientRect(draw_window, &rect)) {
        WARN("GetClientRect failed.\n");
        rect.right = 640;
        rect.bottom = 480;
    }

    if (params->BackBufferWidth == 0) {
        params->BackBufferWidth = rect.right;
    }
    if (params->BackBufferHeight == 0) {
        params->BackBufferHeight = rect.bottom;
    }
    This->params = *params;
    strcpyW(This->devname, devname);

    *out = This;

    return D3D_OK;
}

struct DRI2PresentGroup
{
    /* COM vtable */
    void *vtable;
    /* IUnknown reference count */
    LONG refs;

    struct DRI2Present **present_backends;
    unsigned npresent_backends;
};

static ULONG WINAPI
DRI2PresentGroup_AddRef( struct DRI2PresentGroup *This )
{
    ULONG refs = InterlockedIncrement(&This->refs);
    TRACE("%p increasing refcount to %u.\n", This, refs);
    return refs;
}

static ULONG WINAPI
DRI2PresentGroup_Release( struct DRI2PresentGroup *This )
{
    ULONG refs = InterlockedDecrement(&This->refs);
    TRACE("%p decreasing refcount to %u.\n", This, refs);
    if (refs == 0) {
        unsigned i;
        if (This->present_backends) {
            for (i = 0; i < This->npresent_backends; ++i) {
                DRI2Present_Release(This->present_backends[i]);
            }
            HeapFree(GetProcessHeap(), 0, This->present_backends);
        }
        HeapFree(GetProcessHeap(), 0, This);
    }
    return refs;
}

static HRESULT WINAPI
DRI2PresentGroup_QueryInterface( struct DRI2PresentGroup *This,
                                 REFIID riid,
                                 void **ppvObject )
{
    if (!ppvObject) { return E_POINTER; }
    if (IsEqualGUID(&IID_ID3DPresentGroup, riid) ||
        IsEqualGUID(&IID_IUnknown, riid)) {
        *ppvObject = This;
        DRI2PresentGroup_AddRef(This);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *ppvObject = NULL;

    return E_NOINTERFACE;
}

static UINT WINAPI
DRI2PresentGroup_GetMultiheadCount( struct DRI2PresentGroup *This )
{
    FIXME("(%p), stub!\n", This);
    return 1;
}

static HRESULT WINAPI
DRI2PresentGroup_GetPresent( struct DRI2PresentGroup *This,
                             UINT Index,
                             ID3DPresent **ppPresent )
{
    if (Index >= DRI2PresentGroup_GetMultiheadCount(This)) {
        ERR("Index >= MultiHeadCount\n");
        return D3DERR_INVALIDCALL;
    }
    DRI2Present_AddRef(This->present_backends[Index]);
    *ppPresent = (ID3DPresent *)This->present_backends[Index];

    return D3D_OK;
}

static HRESULT WINAPI
DRI2PresentGroup_CreateAdditionalPresent( struct DRI2PresentGroup *This,
                                          D3DPRESENT_PARAMETERS *pPresentationParameters,
                                          ID3DPresent **ppPresent )
{
    FIXME("(%p, %p, %p), stub!\n", This, pPresentationParameters, ppPresent);
    return D3DERR_INVALIDCALL;
}

static ID3DPresentGroupVtbl DRI2PresentGroup_vtable = {
    (void *)DRI2PresentGroup_QueryInterface,
    (void *)DRI2PresentGroup_AddRef,
    (void *)DRI2PresentGroup_Release,
    (void *)DRI2PresentGroup_GetMultiheadCount,
    (void *)DRI2PresentGroup_GetPresent,
    (void *)DRI2PresentGroup_CreateAdditionalPresent
};

static HRESULT
dri2_create_present_group( const WCHAR *device_name,
                           UINT adapter,
                           HWND focus_wnd,
                           D3DPRESENT_PARAMETERS *params,
                           unsigned nparams,
                           ID3DPresentGroup **group )
{
    struct DRI2PresentGroup *This =
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                  sizeof(struct DRI2PresentGroup));
    DISPLAY_DEVICEW dd;
    HRESULT hr;
    unsigned i;

    if (!This) {
        ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }

    This->vtable = &DRI2PresentGroup_vtable;
    This->refs = 1;
    This->npresent_backends = nparams;
    This->present_backends = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       This->npresent_backends *
                                       sizeof(struct DRI2Present *));
    if (!This->present_backends) {
        DRI2PresentGroup_Release(This);
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
        hr = DRI2Present_new(gdi_display, dd.DeviceName, &params[i],
                             focus_wnd, &This->present_backends[i]);
        if (FAILED(hr)) {
            DRI2PresentGroup_Release(This);
            return hr;
        }
    }

    *group = (ID3DPresentGroup *)This;
    TRACE("Returning %p\n", *group);

    return D3D_OK;
}

static HRESULT
dri2_create_adapter9( HDC hdc,
                      ID3DAdapter9 **out )
{
    struct x11drv_escape_get_drawable extesc = { X11DRV_GET_DRAWABLE };
    drm_auth_t auth;
    Window root;
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

    { /* XGetGeometry */
        unsigned udummy;
        int dummy;

        if (!XGetGeometry(gdi_display, extesc.drawable, &root, &dummy, &dummy,
                          &udummy, &udummy, &udummy, &udummy)) {
            WARN("XGetGeometry: Unable to get root window (drawable=%u)\n",
                 (unsigned)extesc.drawable);
            root = (Window)extesc.drawable; /* cross your fingers */
        }
    }

    { /* DRI2Connect */
        char *driver, *device;

        if (!DRI2Connect(gdi_display, root, DRI2DriverDRI, &driver, &device)) {
            WARN("DRI2Connect: Unable to connect DRI2 (window=%u)\n",
                 (unsigned)root);
            return D3DERR_DRIVERINTERNALERROR;
        }

        fd = open(device, O_RDWR);
        if (fd < 0) {
            WARN("Failed to open drm fd: %s (%s)\n", strerror(errno), device);
            HeapFree(GetProcessHeap(), 0, driver);
            HeapFree(GetProcessHeap(), 0, device);
            return D3DERR_DRIVERINTERNALERROR;
        }

        /* authenticate */
        if (ioctl(fd, DRM_IOCTL_GET_MAGIC, &auth) != 0) {
            WARN("DRM_IOCTL_GET_MAGIC failed: %s (%s)\n",
                 strerror(errno), device);
            HeapFree(GetProcessHeap(), 0, driver);
            HeapFree(GetProcessHeap(), 0, device);
            return D3DERR_DRIVERINTERNALERROR;
        }

        TRACE("Associated `%s' with fd %d opened from `%s'\n",
              driver, fd, device);

        HeapFree(GetProcessHeap(), 0, driver);
        HeapFree(GetProcessHeap(), 0, device);
    }

    if (!DRI2Authenticate(gdi_display, root, auth.magic)) {
        WARN("DRI2Authenticate failed (fd=%d)\n", fd);
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

    /* query DRI2 */
    d3d_info.dri2_major = DRI2_MAJOR;
    d3d_info.dri2_minor = DRI2_MINOR;
    if (!DRI2QueryExtension(gdi_display)) {
        ERR("Xserver doesn't support DRI2.\n");
        return D3DERR_DRIVERINTERNALERROR;
    }
    if (!DRI2QueryVersion(gdi_display, &d3d_info.dri2_major,
                          &d3d_info.dri2_minor)) {
        ERR("Unable to query DRI2 extension.\n");
        return D3DERR_DRIVERINTERNALERROR;
    }
    TRACE("Got DRI2 version %u.%u\n", d3d_info.dri2_major, d3d_info.dri2_minor);

    /* query XFixes */
    if (!pXFixesQueryVersion(gdi_display, &xfmaj, &xfmin)) {
        ERR("Unable to query XFixes extension.\n");
        return D3DERR_DRIVERINTERNALERROR;
    }
    TRACE("Got XFixes version %u.%u\n", xfmaj, xfmin);

    return TRUE;

cleanup:
    ERR("Native Direct3D9 will be unavailable.\n");
    if (handle) {
        wine_dlclose(handle, NULL, 0);
        handle = NULL;
    }

    return FALSE;
}

static struct d3dadapter_funcs dri2_driver = {
    dri2_create_present_group,          /* create_present_group */
    dri2_create_adapter9,               /* create_adapter9 */
};

struct d3dadapter_funcs *
get_d3d_dri2_driver(UINT version)
{
    if (version != WINE_D3DADAPTER_DRIVER_VERSION) {
        ERR("Version mismatch. d3d* wants %u but winex11 has "
            "version %u\n", version, WINE_D3DADAPTER_DRIVER_VERSION);
        return NULL;
    }
    if (has_d3dadapter()) { return &dri2_driver; }
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
    return FALSE;
}

struct d3dadapter_funcs *
get_d3d_dri2_driver(UINT version)
{
    return NULL;
}

#endif /* defined(SONAME_LIBXEXT) && \
          defined(SONAME_LIBXFIXES) && \
          defined(SONAME_LIBD3DADAPTER9) */
