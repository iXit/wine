/*
 * d3dadapter display driver definitions
 *
 * Copyright (c) 2013 Joakim Sindholt
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

#ifndef __WINE_D3DADAPTER_H
#define __WINE_D3DADAPTER_H

#ifndef __WINE_CONFIG_H
# error You must include config.h to use this header
#endif

#ifdef SONAME_D3DADAPTER9

#include <d3dadapter/d3dadapter9.h>

#define WINE_D3DADAPTER_DRIVER_VERSION 0

struct d3dadapter_funcs
{
    HRESULT (*create_present_group)(const WCHAR *device_name, UINT adapter, HWND focus, D3DPRESENT_PARAMETERS *params, unsigned nparams, ID3DPresentGroup **group);
    HRESULT (*create_adapter9)(HDC hdc, ID3DAdapter9 **adapter);
};

#endif /* SONAME_D3DADAPTER9 */

#endif /* __WINE_D3DADAPTER_H */
