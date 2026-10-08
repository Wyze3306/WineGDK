/*
 * Copyright 2026 Olivia Ryan
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

#ifndef TOKEN_H
#define TOKEN_H

#include "../../../private.h"

#include <errno.h>
#include <winhttp.h>
#include "time.h"

struct token
{
    time_t expiry;
    LPCSTR content;
    UINT32 size;
};

HRESULT RefreshOAuth( LPCSTR client_id, LPCSTR refresh_token, time_t *new_expiry, HSTRING *new_refresh_token, HSTRING *new_oauth_token );
HRESULT RequestUserToken( HSTRING oauth_token, HSTRING *token, XUserLocalId *localId );
HRESULT RequestXstsToken( HSTRING user_token, HSTRING *token, UINT64 *xuid, XUserAgeGroup *age_group, LPSTR gamertag, SIZE_T gamertag_size );
HRESULT RequestXstsTokenForRelyingParty( HSTRING user_token, LPCSTR relying_party,
                                         HSTRING *token, UINT64 *uhs );
/* SISU single-call auth: takes the MSA OAuth token + the previously-issued
 * device token and asks sisu.xboxlive.com/authorize for an XSTS token
 * **bound to the Minecraft title** (via client_id 0000000048183522 which
 * Microsoft has on file for the Bedrock title id).  This is the path
 * gophertunnel / ProxyPass use to authenticate against PlayFab without a
 * separate title.auth call (which always returns 401 here because we
 * can't mint a title-credential RPS ticket).  Returns the XSTS-equivalent
 * Token from AuthorizationToken — same XBL3.0 shape as
 * RequestXstsTokenForRelyingParty, just title-bound. */
HRESULT RequestSisuAuthorize( LPCSTR client_id, HSTRING oauth_token,
                              HSTRING device_token, LPCSTR relying_party,
                              HSTRING *xsts_token, UINT64 *uhs );
HRESULT HSTRINGToMultiByte( HSTRING hstr, LPSTR *str, UINT32 *str_len );
HRESULT HttpRequest( LPCWSTR method, LPCWSTR host, LPCWSTR path, LPSTR data,
                     LPCWSTR headers, LPCWSTR *accept, LPSTR *buffer, SIZE_T *size );
HRESULT ParseJsonObject( LPCSTR str, UINT32 str_size, IJsonObject **object );

#endif
