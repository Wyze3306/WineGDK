/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XSystem
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

#include "HTTPClient.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct httpclient_request
{
    HINTERNET session;
    HINTERNET connect;
    HINTERNET request;
};

static void httpclient_CloseRequest( struct httpclient_request *client )
{
    if ( client->request ) WinHttpCloseHandle( client->request );
    if ( client->connect ) WinHttpCloseHandle( client->connect );
    if ( client->session ) WinHttpCloseHandle( client->session );
    memset( client, 0, sizeof(*client) );
}

static HRESULT httpclient_SendRequest( URL_COMPONENTS uc, struct httpclient_request *client )
{
    DWORD reqFlags;
    LPWSTR hostName = NULL;
    LPWSTR urlPath = NULL;
    HRESULT status = S_OK;

    if ( !client ) return E_POINTER;
    memset( client, 0, sizeof(*client) );

    TRACE( "uc %p, client %p\n", &uc, client );

    hostName = HeapAlloc( GetProcessHeap(), 0, (uc.dwHostNameLength + 1) * sizeof(WCHAR) );
    if ( !hostName ) 
    { 
        status = E_OUTOFMEMORY; goto _CLEANUP; 
    }
    wcsncpy_s( hostName, uc.dwHostNameLength + 1, uc.lpszHostName, uc.dwHostNameLength );
    hostName[uc.dwHostNameLength] = L'\0';

    if ( uc.dwUrlPathLength == 0 ) 
    {
        urlPath = HeapAlloc( GetProcessHeap(), 0, 2 * sizeof(WCHAR) );
        if ( !urlPath ) 
        { 
            status = E_OUTOFMEMORY; 
            goto _CLEANUP; 
        }
        wcscpy_s( urlPath, 2, L"/" );
    } else 
    {
        urlPath = HeapAlloc( GetProcessHeap(), 0, (uc.dwUrlPathLength + 1) * sizeof(WCHAR) );
        if ( !urlPath ) 
        { 
            status = E_OUTOFMEMORY; 
            goto _CLEANUP; 
        }
        wcsncpy_s( urlPath, uc.dwUrlPathLength + 1, uc.lpszUrlPath, uc.dwUrlPathLength );
        urlPath[uc.dwUrlPathLength] = L'\0';
    }

    client->session = WinHttpOpen( L"curl/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
    if ( !client->session )
    {
        status = HRESULT_FROM_WIN32( GetLastError() );
        goto _CLEANUP;
    }

    client->connect = WinHttpConnect( client->session, hostName, uc.nPort, 0 );
    if ( !client->connect )
    {
        status = HRESULT_FROM_WIN32( GetLastError() );
        goto _CLEANUP;
    }

    reqFlags = ( uc.nScheme == INTERNET_SCHEME_HTTPS ) ? WINHTTP_FLAG_SECURE : 0;
    client->request = WinHttpOpenRequest( client->connect, L"GET", urlPath, NULL,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags );
    if ( !client->request )
    {
        status = HRESULT_FROM_WIN32( GetLastError() );
        goto _CLEANUP;
    }

    if ( !WinHttpSendRequest( client->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) )
    {
        status = HRESULT_FROM_WIN32( GetLastError() );
        goto _CLEANUP;
    }

    /* The TLS handshake (and therefore the server certificate) is complete the
     * moment WinHttpSendRequest returns — the cert is captured by
     * netconn_get_certificate at connection time and stays queryable via
     * WINHTTP_OPTION_SERVER_CERT_CONTEXT regardless of the HTTP response.
     *
     * Reading the response body is NOT required to obtain the pinning info
     * (thumbprints + protocol flags) this call exists for, and on Wine 11.1 it
     * actively breaks us: secur32's GnuTLS backend fails to decrypt the first
     * response record from Azure-fronted hosts (*.playfabapi.com,
     * client.discovery.minecraft-services.net, *.events.data.microsoft.com)
     * with schan_DecryptMessage 0x80090304 (SEC_E_INTERNAL_ERROR). Treating
     * that as fatal made XNetworkingQuerySecurityInformationForUrl fail, which
     * Minecraft's libHttpClient gates every PlayFab call on — so the real
     * LoginWithXbox POST (which runs over XCurl/libcurl, immune to the bug)
     * never fired and the Servers list / sign-in looped forever.
     *
     * Make the response read best-effort: log a failure and carry on so the
     * caller can still pull the certificate it needs. */
    if ( !WinHttpReceiveResponse( client->request, NULL ) )
        WARN( "WinHttpReceiveResponse failed (%#lx) — proceeding with cert only\n", GetLastError() );

_CLEANUP:
    if ( FAILED( status ) ) httpclient_CloseRequest( client );
    if ( hostName ) HeapFree( GetProcessHeap(), 0, hostName );
    if ( urlPath ) HeapFree( GetProcessHeap(), 0, urlPath );
    return status;
}

static HRESULT httpclient_ObtainSecurityProtocolFlags( HINTERNET inetRequest, UINT32 *flags )
{
    DWORD secInfoSize = sizeof( WINHTTP_SECURITY_INFO );
    HRESULT status = S_OK;
    PWINHTTP_SECURITY_INFO securityInfo;

    TRACE( "inetRequest %p, flags %p\n", inetRequest, flags );

    if ( !flags ) return E_POINTER;
    securityInfo = CoTaskMemAlloc( secInfoSize );
    if ( !securityInfo ) return E_OUTOFMEMORY;

    if ( !WinHttpQueryOption( inetRequest, WINHTTP_OPTION_SECURITY_INFO, securityInfo, &secInfoSize ) )
    {
        status = HRESULT_FROM_WIN32( GetLastError() );
        goto _CLEANUP;
    }

    *flags = securityInfo->ConnectionInfo.dwProtocol;

_CLEANUP:
    CoTaskMemFree( securityInfo );
    return status;
}

static HRESULT httpclient_ObtainServerCertificate( HINTERNET inetRequest, PCERT_CONTEXT *context )
{
    DWORD certContextSize = sizeof( PCCERT_CONTEXT );
    HRESULT status = S_OK;

    TRACE( "inetRequest %p, context %p\n", inetRequest, context );

    if ( !context ) return E_POINTER;
    *context = NULL;

    if ( !WinHttpQueryOption( inetRequest, WINHTTP_OPTION_SERVER_CERT_CONTEXT, context, &certContextSize ) )
    {
        status = HRESULT_FROM_WIN32( GetLastError() );
        goto _CLEANUP;
    }

_CLEANUP:
    return status;
}

static HRESULT httpclient_ObtainThumbprints( HINTERNET inetRequest, SIZE_T *thumbprintCount, XNetworkingThumbprint **out )
{
    SIZE_T idx = 0;
    HRESULT status = S_OK;
    PCERT_CONTEXT certContext = NULL;
    CERT_CHAIN_PARA chainPara = { .cbSize = sizeof(CERT_CHAIN_PARA) };
    PCCERT_CHAIN_CONTEXT chainContext = NULL;
    XNetworkingThumbprint *thumbprints = NULL;

    TRACE( "inetRequest %p, thumbprintCount %p, out %p\n", inetRequest, thumbprintCount, out );

    if ( !thumbprintCount || !out ) return E_POINTER;
    *thumbprintCount = 0;
    *out = NULL;

    status = httpclient_ObtainServerCertificate( inetRequest, &certContext );
    if ( FAILED( status ) ) goto _CLEANUP;

    if ( !CertGetCertificateChain( NULL, certContext, NULL, certContext->hCertStore, &chainPara, 0, NULL, &chainContext ) )
    {
        status = HRESULT_FROM_WIN32( GetLastError() );
        goto _CLEANUP;
    }

    thumbprints = (XNetworkingThumbprint *)calloc( 3, sizeof(XNetworkingThumbprint) );
    if ( !thumbprints )
    {
        status = E_OUTOFMEMORY;
        goto _CLEANUP;
    }

#define TRY_ADD_THUMBPRINT_FROM_CERT(pCertCtx, tp)                                                                  \
    do {                                                                                                            \
        if ( pCertCtx ) {                                                                                           \
            DWORD cb = 0;                                                                                           \
            /* Wine's WinHTTP only supports SHA1 */                                                                 \
            if ( CertGetCertificateContextProperty( pCertCtx, CERT_SHA1_HASH_PROP_ID, NULL, &cb ) )                 \
            {                                                                                                       \
                UINT8* buf = (UINT8*)malloc( cb );                                                                  \
                if ( buf && CertGetCertificateContextProperty( pCertCtx, CERT_SHA1_HASH_PROP_ID, buf, &cb ) )       \
                {                                                                                                   \
                    thumbprints[idx].thumbprintType = (tp);                                                         \
                    thumbprints[idx].thumbprintBufferByteCount = (SIZE_T)cb;                                        \
                    thumbprints[idx].thumbprintBuffer = buf;                                                        \
                    idx++;                                                                                          \
                } else { free( buf ); }                                                                             \
            }                                                                                                       \
        }                                                                                                           \
    } while( FALSE )

    if ( chainContext && chainContext->cChain > 0 && chainContext->rgpChain[0] ) 
    {
        PCERT_SIMPLE_CHAIN simple = chainContext->rgpChain[0];
        DWORD numElements = simple->cElement;
        if ( numElements >= 1 ) 
        {
            // leaf = element[0]
            TRY_ADD_THUMBPRINT_FROM_CERT( simple->rgpElement[0]->pCertContext, ThumbprintType_Leaf );
        }
        if ( numElements >= 2 ) 
        {
            // issuer = element[1] (immediate issuer of leaf)
            TRY_ADD_THUMBPRINT_FROM_CERT( simple->rgpElement[1]->pCertContext, ThumbprintType_Issuer );
        }
        if ( numElements >= 1 ) 
        {
            // root = last element
            TRY_ADD_THUMBPRINT_FROM_CERT( simple->rgpElement[numElements - 1]->pCertContext, ThumbprintType_Root );
        }
    } else 
    {
        // chain not available: at least try server cert (leaf)
        TRY_ADD_THUMBPRINT_FROM_CERT( certContext, ThumbprintType_Leaf );
    }

    if ( idx > 0 )
    {
        *out = thumbprints;
        *thumbprintCount = idx;
        thumbprints = NULL;
    }

#undef TRY_ADD_THUMBPRINT_FROM_CERT

_CLEANUP:
    if ( thumbprints )
    {
        for ( idx = 0; idx < 3; idx++ ) free( thumbprints[idx].thumbprintBuffer );
        free( thumbprints );
    }
    if ( chainContext ) CertFreeCertificateChain( chainContext );
    if ( certContext ) CertFreeCertificateContext( certContext );

    return status;
}

HRESULT httpclient_ObtainSecurityInformationForUrl( LPCWSTR url, BYTE **outBuffer, SIZE_T *outBufferByteCount )
{
    LPBYTE buffer = NULL;
    LPBYTE bufferLoc = NULL;
    SIZE_T iterator;
    SIZE_T totalBufferSize;
    SIZE_T thumbprintBytes = 0;
    HRESULT status = S_OK;
    struct httpclient_request client = {0};
    URL_COMPONENTS uc = { .dwStructSize = sizeof(URL_COMPONENTS), 
        .dwSchemeLength = (DWORD)-1, .dwHostNameLength = (DWORD)-1, .dwUrlPathLength = (DWORD)-1, .dwExtraInfoLength = (DWORD)-1 };
    XNetworkingSecurityInformation *information = NULL;
    LPWSTR httpUrl = NULL;

    TRACE( "url %s\n", debugstr_w( url ) );

    if ( !url || !outBuffer || !outBufferByteCount ) return E_POINTER;
    *outBuffer = NULL;
    *outBufferByteCount = 0;

    /* Minecraft asks for the pinning info of its Real-Time-Activity WebSocket
     * endpoint (wss://signal-*.franchise.minecraft-services.net/...). Wine's
     * WinHttpCrackUrl only understands the http/https schemes — handed a "wss"
     * or "ws" URL it fails and leaves the components empty, so SendRequest
     * connects to an empty host, no certificate/thumbprints come back, and
     * XNetworkingQuerySecurityInformationForUrl fails. Minecraft then never
     * opens the RTA socket, so presence/multiplayer never comes up and the
     * "Play" button stays greyed out. The cert is identical over the
     * equivalent https origin, so rewrite the scheme before cracking. */
    if ( url )
    {
        SIZE_T urlLen = wcslen( url );
        const WCHAR *rest = NULL;
        if ( _wcsnicmp( url, L"wss://", 6 ) == 0 )
            rest = url + 6;
        else if ( _wcsnicmp( url, L"ws://", 5 ) == 0 )
            rest = url + 5;
        if ( rest )
        {
            /* "https://" (8) or "http://" (7) + rest + NUL */
            SIZE_T n = urlLen + 8 + 1;
            httpUrl = HeapAlloc( GetProcessHeap(), 0, n * sizeof(WCHAR) );
            if ( httpUrl )
            {
                wcscpy_s( httpUrl, n, ( rest == url + 6 ) ? L"https://" : L"http://" );
                wcscat_s( httpUrl, n, rest );
            }
            else
            {
                status = E_OUTOFMEMORY;
                goto _CLEANUP;
            }
        }
    }

    if ( !WinHttpCrackUrl( httpUrl ? httpUrl : url, 0, 0, &uc ) )
    {
        status = HRESULT_FROM_WIN32( GetLastError() );
        goto _CLEANUP;
    }

    status = httpclient_SendRequest( uc, &client );
    if ( FAILED( status ) ) goto _CLEANUP;

    information = calloc( 1, sizeof(*information) );
    if ( !information )
    {
        status = E_OUTOFMEMORY;
        goto _CLEANUP;
    }

    status = httpclient_ObtainThumbprints( client.request, &information->thumbprintCount,
            &information->thumbprints );
    if ( FAILED( status ) ) goto _CLEANUP;

    // TODO: Security Protocol Flags are not supported by wine's WinHTTP
    status = httpclient_ObtainSecurityProtocolFlags( client.request,
            &information->enabledHttpSecurityProtocolFlags );
    if ( FAILED( status ) )
    {
        information->enabledHttpSecurityProtocolFlags = WINHTTP_FLAG_SECURE_PROTOCOL_ALL | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        status = S_OK;
    }

    for ( iterator = 0; iterator < information->thumbprintCount; iterator++ )
        thumbprintBytes += information->thumbprints[iterator].thumbprintBufferByteCount;

    totalBufferSize = sizeof(XNetworkingSecurityInformation) + (information->thumbprintCount * sizeof(XNetworkingThumbprint)) + thumbprintBytes;

    buffer = (LPBYTE)malloc( totalBufferSize );
    if ( !buffer )
    {
        status = E_OUTOFMEMORY;
        goto _CLEANUP;
    }

    {
        XNetworkingSecurityInformation *packed = (XNetworkingSecurityInformation *)buffer;
        XNetworkingThumbprint *packedThumbprints = (XNetworkingThumbprint *)(buffer + sizeof(*packed));

        packed->enabledHttpSecurityProtocolFlags = information->enabledHttpSecurityProtocolFlags;
        packed->thumbprintCount = information->thumbprintCount;
        packed->thumbprints = packedThumbprints;
        bufferLoc = (BYTE *)(packedThumbprints + information->thumbprintCount);

        for ( iterator = 0; iterator < information->thumbprintCount; iterator++ )
        {
            packedThumbprints[iterator].thumbprintType = information->thumbprints[iterator].thumbprintType;
            packedThumbprints[iterator].thumbprintBufferByteCount =
                    information->thumbprints[iterator].thumbprintBufferByteCount;
            packedThumbprints[iterator].thumbprintBuffer = bufferLoc;
            memcpy( bufferLoc, information->thumbprints[iterator].thumbprintBuffer,
                    information->thumbprints[iterator].thumbprintBufferByteCount );
            bufferLoc += information->thumbprints[iterator].thumbprintBufferByteCount;
        }
    }

    *outBuffer = buffer;
    *outBufferByteCount = totalBufferSize;

_CLEANUP:
    if ( FAILED( status ) )
    {
        free( buffer );
    }
    httpclient_CloseRequest( &client );
    if ( httpUrl ) HeapFree( GetProcessHeap(), 0, httpUrl );
    if ( information )
    {
        for ( iterator = 0; iterator < information->thumbprintCount; iterator++ )
            free( information->thumbprints[iterator].thumbprintBuffer );
        free( information->thumbprints );
        free( information );
    }
    return status;
}
