/*
 * Xbox Game runtime Library
 *  GDK Component: Internal Initialization
 * 
 * Written by Weather
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

#include "InitInternalGDKC.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static BOOLEAN WINAPI GDKCIgnoreVersionMismatch( void )
{
    DWORD valueSize = sizeof(DWORD);
    DWORD value = 0;
    LSTATUS result;

    /* RegGetValueW returns a Win32 error code, which FAILED() never sees as
     * a failure, and it needs the size of the buffer it may write. */
    result = RegGetValueW( HKEY_LOCAL_MACHINE,
                           L"Software\\Microsoft\\GamingServices",
                           L"IgnoreVersionMismatch",
                           RRF_RT_REG_DWORD,
                           NULL,
                           &value,
                           &valueSize );

    return result == ERROR_SUCCESS && value != 0;
}

HRESULT WINAPI GDKC_InitAPI( 
    ULONG gdkVer, 
    ULONG gsVer, 
    CHAR mode,
    INITIALIZE_OPTIONS *options
) {
    HRESULT status = S_OK;

    TRACE("gdkVer %ld, gsVer %ld, mode %d, options %p\n", gdkVer, gsVer, mode, options);

    /* A title built against a newer Gaming Services still runs on this one:
     * a call this runtime does not implement fails on its own, as every
     * other unimplemented call does. Refusing the title here instead quits
     * it before its window opens, with nothing on screen to say why, and
     * there is no Gaming Services update to install. */
    if ( gdkVer >= GDKC_VERSION && gsVer > GAMING_SERVICES_VERSION
         && !GDKCIgnoreVersionMismatch() )
    {
        WARN("title targets GDK %ld and Gaming Services %ld; this runtime is GDK %ld and "
             "Gaming Services %ld, continuing\n",
             gdkVer, gsVer, GDKC_VERSION, GAMING_SERVICES_VERSION );
    }

    /* INITIALIZE_OPTIONS is an opaque ABI-owned structure here.  Do not
     * inspect it: older and newer Gaming Runtime versions use different
     * layouts.  The packaged game's identity is discovered from the
     * executable location instead. */
    status = WineGDKLoadGameConfig();
    if (status == E_GAME_MISSING_GAME_CONFIG)
    {
        WARN("continuing without MicrosoftGame.Config; XGameGetXboxTitleId will report ERROR_NOT_FOUND\n");
        status = S_OK;
    }
    else if (FAILED( status ))
    {
        ERR("MicrosoftGame.Config initialization failed, hr %#lx\n", status);
        return status;
    }

    return status;

    // TODO: Game Specific Initialization
}
