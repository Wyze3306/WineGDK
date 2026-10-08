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

/*
 * Xbox Game runtime Library
 * GDK Component: System API -> XUser
 */

#ifndef XUSER_H
#define XUSER_H

#include "../../../private.h"
#include "Token.h"

struct x_user
{
    IXUserImpl IXUserImpl_iface;
    IXUserGamertag IXUserGamertag_iface;
    LONG ref;
    BOOLEAN is_provider;

    UINT64 xuid;
    XUserLocalId local_id;
    XUserAgeGroup age_group;

    time_t oauth_token_expiry;
    time_t user_token_expiry;
    time_t xsts_token_expiry;
    HSTRING refresh_token;
    HSTRING oauth_token;
    HSTRING user_token;
    HSTRING xsts_token;
    CHAR gamertag[128];
    CHAR modern_gamertag[128];
    CHAR modern_gamertag_suffix[32];
    CHAR unique_modern_gamertag[160];

    /* User-only XSTS token for the Windows Achievements service.  The
     * launcher mints it without the Android SISU AppId and retains the
     * matching user hash separately from the title-bound profile token. */
    HSTRING achievements_token;
    UINT64 achievements_uhs;
    time_t achievements_expiry;
    SRWLOCK achievements_lock;

    /* Canonical, space-separated privilege IDs from the XSTS ``prv`` claim.
     * Presence is tracked independently because old launcher caches carry no
     * claim and retain the permissive compatibility fallback, while an
     * explicit empty claim grants no privileges. */
    HSTRING xbl_privileges;
    BOOLEAN xbl_privileges_present;

    /* Cached SISU AuthorizationToken (PlayFab/multiplayer audience).
     * SISU's /authorize is rate-limited per AppId — calling it per
     * outgoing HTTP request earned a long string of HTTP 4xx after
     * the first hit.  Mint once per RP and reuse until ~30 s before
     * NotAfter; rebuild after expiry. */
    HSTRING sisu_token;
    UINT64 sisu_uhs;
    time_t sisu_expiry;
    CHAR sisu_rp[256];

    /* Cached SISU token for the multiplayer RP (https://multiplayer.minecraft.net/),
     * pre-minted by the launcher so joining a server doesn't need a live SISU
     * call (which RSTs under Wine GnuTLS and leaves the join token empty). */
    HSTRING mp_token;
    UINT64 mp_uhs;
    time_t mp_expiry;
    CHAR mp_rp[256];

    /* Cached SISU token for Bedrock Realms.  Requests now use
     * *.realms.minecraft-services.net, while the accepted XSTS audience
     * remains https://pocket.realms.minecraft.net/. */
    HSTRING realms_token;
    UINT64 realms_uhs;
    time_t realms_expiry;
    CHAR realms_rp[256];

    /* Cached SISU token for the marketplace/licensing RP
     * (http://licensing.xboxlive.com), pre-minted by the launcher so the
     * in-game Marketplace's catalog and entitlement calls
     * (collections/purchase.mp.microsoft.com, inventory/licensing.xboxlive.com)
     * resolve to a valid XSTS audience without a live SISU call. */
    HSTRING lic_token;
    UINT64 lic_uhs;
    time_t lic_expiry;
    CHAR lic_rp[256];
};

#endif
