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

#include "Token.h"
#include "DeviceAuth.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

#define GetJsonValue( obj_type, ret_type )                                                          \
static inline HRESULT GetJson##obj_type##Value( IJsonObject *object, LPCWSTR key, ret_type value )  \
{                                                                                                   \
    HSTRING_HEADER key_hdr;                                                                         \
    HSTRING key_hstr;                                                                               \
    HRESULT hr;                                                                                     \
                                                                                                    \
    if (FAILED( hr = WindowsCreateStringReference( key, wcslen( key ), &key_hdr, &key_hstr ) ))     \
        return hr;                                                                                  \
                                                                                                    \
    if (FAILED( hr = IJsonObject_GetNamed##obj_type( object, key_hstr, value ) ))                   \
        return hr;                                                                                  \
                                                                                                    \
    return S_OK;                                                                                    \
}

GetJsonValue( Array, IJsonArray** );
GetJsonValue( Number, DOUBLE* );
GetJsonValue( Object, IJsonObject** );
GetJsonValue( String, HSTRING* );

HRESULT HSTRINGToMultiByte( HSTRING hstr, LPSTR *str, UINT32 *str_len )
{
    UINT32 wstr_len;
    LPCWSTR wstr = WindowsGetStringRawBuffer( hstr, &wstr_len );

    if (!(*str_len = WideCharToMultiByte( CP_UTF8, 0, wstr, wstr_len, NULL, 0, NULL, NULL )))
        return HRESULT_FROM_WIN32( GetLastError() );

    if (!(*str = calloc( 1, *str_len ))) return E_OUTOFMEMORY;

    if (!(*str_len = WideCharToMultiByte( CP_UTF8, 0, wstr, wstr_len, *str, *str_len, NULL, NULL )))
    {
        free( *str );
        *str = NULL;
        return HRESULT_FROM_WIN32( GetLastError() );
    }

    return S_OK;
}

HRESULT HttpRequest( LPCWSTR method, LPCWSTR domain, LPCWSTR object, LPSTR data, LPCWSTR headers, LPCWSTR *accept, LPSTR *buffer, SIZE_T *bufferSize )
{
    HINTERNET connection = NULL;
    DWORD size = sizeof( DWORD );
    HINTERNET session = NULL;
    HINTERNET request = NULL;
    HRESULT hr = S_OK;
    DWORD status;

    if (!(session = WinHttpOpen(
        L"WineGDK/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    ))) return HRESULT_FROM_WIN32( GetLastError() );

    if (!(connection = WinHttpConnect(
        session,
        domain,
        INTERNET_DEFAULT_HTTPS_PORT,
        0
    ))) hr = HRESULT_FROM_WIN32( GetLastError() );

    if (SUCCEEDED( hr ) && !(request = WinHttpOpenRequest(
        connection,
        method,
        object,
        NULL,
        WINHTTP_NO_REFERER,
        accept,
        WINHTTP_FLAG_SECURE
    ))) hr = HRESULT_FROM_WIN32( GetLastError() );

    if (SUCCEEDED( hr ) && !WinHttpSendRequest(
        request,
        headers,
        -1,
        data,
        strlen( data ),
        strlen( data ),
        0
    )) hr = HRESULT_FROM_WIN32( GetLastError() );

    if (SUCCEEDED( hr ) && !WinHttpReceiveResponse( request, NULL ))
        hr = HRESULT_FROM_WIN32( GetLastError() );

    if (SUCCEEDED( hr ) && !WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &size,
        WINHTTP_NO_HEADER_INDEX
    )) hr = HRESULT_FROM_WIN32( GetLastError() );

    if (SUCCEEDED( hr ) && status / 100 != 2) hr = E_FAIL;

    /* buffer response data */
    *buffer = NULL;
    *bufferSize = 0;
    if (SUCCEEDED( hr ))
    {
        do
        {
            if (!(WinHttpQueryDataAvailable( request, &size )))
            {
                hr = HRESULT_FROM_WIN32( GetLastError() );
                break;
            }

            if (!size) break;
            if (!(*buffer = realloc( *buffer, *bufferSize + size )))
            {
                hr = E_OUTOFMEMORY;
                break;
            }

            if (!(WinHttpReadData( request, *buffer + *bufferSize, size, &size )))
            {
                hr = HRESULT_FROM_WIN32( GetLastError() );
                break;
            }

            *bufferSize += size;
        }
        while (size);
    }

    if (connection) WinHttpCloseHandle( connection );
    if (request) WinHttpCloseHandle( request );
    if (session) WinHttpCloseHandle( session );
    if (FAILED( hr ) && *buffer) free( *buffer );

    return hr;
}

HRESULT ParseJsonObject( LPCSTR str, UINT32 str_size, IJsonObject **object )
{
    LPCWSTR class_str = RuntimeClass_Windows_Data_Json_JsonValue;
    IJsonValueStatics *statics;
    HSTRING_HEADER content_hdr;
    HSTRING_HEADER class_hdr;
    IJsonValue *value;
    UINT32 wstr_size;
    HSTRING content;
    HSTRING class;
    LPWSTR wstr;
    HRESULT hr;

    if (!(wstr_size = MultiByteToWideChar( CP_UTF8, 0, str, str_size, NULL, 0 )))
        return HRESULT_FROM_WIN32( GetLastError() );

    if (!(wstr = calloc( wstr_size + 1, sizeof( WCHAR ) )))
        return E_OUTOFMEMORY;

    if (!(wstr_size = MultiByteToWideChar( CP_UTF8, 0, str, str_size, wstr, wstr_size )))
    {
        free( wstr );
        return HRESULT_FROM_WIN32( GetLastError() );
    }

    if (FAILED( hr = WindowsCreateStringReference( wstr, wstr_size, &content_hdr, &content ) ))
    {
        free( wstr );
        return hr;
    }

    if (FAILED( hr = WindowsCreateStringReference( class_str, wcslen( class_str ), &class_hdr, &class ) ))
    {
        free( wstr );
        return hr;
    }

    if (FAILED( hr = RoGetActivationFactory( class, &IID_IJsonValueStatics, (void**)&statics ) ))
    {
        free( wstr );
        return hr;
    }

    hr = IJsonValueStatics_Parse( statics, content, &value );
    IJsonValueStatics_Release( statics );
    free( wstr );
    if (FAILED( hr )) return hr;

    hr = IJsonValue_GetObject( value, object );
    IJsonValue_Release( value );
    if (FAILED( hr )) IJsonObject_Release( *object );

    return hr;
}

HRESULT RefreshOAuth( LPCSTR client_id, LPCSTR refresh_token, time_t *new_expiry, HSTRING *new_refresh_token, HSTRING *new_oauth_token )
{
    LPCSTR template = "grant_type=refresh_token&scope=service::user.auth.xboxlive.com::MBI_SSL&client_id=";
    LPCWSTR accept[] = {L"application/json", NULL};
    IJsonObject *object;
    time_t expiry;
    LPSTR buffer;
    DOUBLE delta;
    SIZE_T size;
    HRESULT hr;
    LPSTR data;

    if (!(data = calloc( strlen( template ) + strlen( client_id ) + strlen( "&refresh_token=" ) + strlen( refresh_token ) + 1, sizeof( CHAR ) )))
        return E_OUTOFMEMORY;

    strcpy( data, template );
    strcat( data, client_id );
    strcat( data, "&refresh_token=" );
    strcat( data, refresh_token );

    hr = HttpRequest(
        L"POST",
        L"login.live.com",
        L"/oauth20_token.srf",
        data,
        L"content-type: application/x-www-form-urlencoded",
        accept,
        &buffer,
        &size
    );

    free( data );
    if (FAILED( hr )) return hr;
    hr = ParseJsonObject( buffer, size, &object );
    free( buffer );
    if (FAILED( hr )) return hr;

    if (FAILED( hr = GetJsonStringValue( object, L"access_token", new_oauth_token ) ))
    {
        IJsonObject_Release( object );
        return hr;
    }

    if (FAILED( hr = GetJsonStringValue( object, L"refresh_token", new_refresh_token ) ))
    {
        IJsonObject_Release( object );
        return hr;
    }

    if (FAILED( hr = GetJsonNumberValue( object, L"expires_in", &delta ) ))
    IJsonObject_Release( object );
    if (FAILED( hr )) return hr;

    if ((expiry = time( NULL )) == -1) return E_FAIL;
    *new_expiry = expiry + delta;

    return S_OK;
}

HRESULT RequestUserToken( HSTRING oauth_token, HSTRING *token, XUserLocalId *local_id )
{
    LPCSTR template = "{\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"AuthMethod\":\"RPS\",\"SiteName\":\"user.auth.xboxlive.com\",\"RpsTicket\":\"";
    LPCWSTR accept[] = {L"application/json", NULL};
    IJsonObject *display_claims;
    UINT32 token_str_len;
    IJsonObject *object;
    UINT32 uhs_str_len;
    LPSTR token_str;
    IJsonArray *xui;
    LPSTR uhs_str;
    LPSTR buffer;
    SIZE_T size;
    HSTRING uhs;
    LPSTR data;
    HRESULT hr;

    if (FAILED( hr = HSTRINGToMultiByte( oauth_token, &token_str, &token_str_len ) ))
        return hr;

    if (!(data = calloc( strlen( template ) + token_str_len + strlen( "\"}}" ) + 1, sizeof( CHAR ) )))
    {
        free( token_str );
        return E_OUTOFMEMORY;
    }

    strcpy( data, template );
    strncat( data, token_str, token_str_len );
    free( token_str );
    strcat( data, "\"}}" );

    hr = HttpRequest(
        L"POST",
        L"user.auth.xboxlive.com",
        L"/user/authenticate",
        data,
        L"content-type: application/json",
        accept,
        &buffer,
        &size
    );

    free( data );
    if (FAILED( hr )) return hr;
    hr = ParseJsonObject( buffer, size, &object );
    free( buffer );
    if (FAILED( hr )) return hr;

    if (FAILED( hr = GetJsonStringValue( object, L"Token", token ) ))
    {
        IJsonObject_Release( object );
        return hr;
    }

    hr = GetJsonObjectValue( object, L"DisplayClaims", &display_claims );
    IJsonObject_Release( object );
    if (FAILED( hr ))
    {
        WindowsDeleteString( *token );
        return hr;
    }

    hr = GetJsonArrayValue( display_claims, L"xui", &xui );
    IJsonObject_Release( display_claims );
    if (FAILED( hr ))
    {
        WindowsDeleteString( *token );
        return hr;
    }

    hr = IJsonArray_GetObjectAt( xui, 0, &object );
    IJsonArray_Release( xui );
    if (FAILED( hr ))
    {
        WindowsDeleteString( *token );
        return hr;
    }

    hr = GetJsonStringValue( object, L"uhs", &uhs );
    IJsonObject_Release( object );
    if (FAILED( hr ))
    {
        WindowsDeleteString( *token );
        return hr;
    }

    hr = HSTRINGToMultiByte( uhs, &uhs_str, &uhs_str_len );
    WindowsDeleteString( uhs );
    if (FAILED( hr ))
    {
        WindowsDeleteString( *token );
        return hr;
    }

    local_id->value = strtoull( uhs_str, NULL, 10 );
    free( uhs_str );
    if (errno == ERANGE)
    {
        WindowsDeleteString( *token );
        errno = 0;
        return E_FAIL;
    }

    return hr;
}

HRESULT RequestXstsToken( HSTRING user_token, HSTRING *token, UINT64 *xuid, XUserAgeGroup *age_group, LPSTR gamertag, SIZE_T gamertag_size )
{
    LPCSTR template = "{\"RelyingParty\":\"http://xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"SandboxId\":\"RETAIL\",\"UserTokens\":[\"";
    LPCWSTR accept[] = {L"application/json", NULL};
    IJsonObject *display_claims;
    UINT32 token_str_len;
    IJsonObject *object;
    UINT32 xid_str_len;
    LPCWSTR agg_str;
    LPSTR token_str;
    IJsonArray *xui;
    UINT32 agg_len;
    LPSTR xid_str;
    LPSTR buffer;
    HSTRING agg;
    SIZE_T size;
    HSTRING xid;
    HRESULT hr;
    LPSTR data;
    HSTRING gtg;

    TRACE( "RequestXstsToken starting\n" );

    if (FAILED( hr = HSTRINGToMultiByte( user_token, &token_str, &token_str_len ) ))
    {
        WARN( "failed to convert user_token to multibyte: 0x%08lx\n", hr );
        return hr;
    }

    if (!(data = calloc( strlen( template ) + token_str_len + strlen( "\"]}}" ) + 1, sizeof( CHAR ) )))
    {
        free( token_str );
        return E_OUTOFMEMORY;
    }

    strcpy( data, template );
    strncat(data, token_str, token_str_len);
    free( token_str );
    strcat( data, "\"]}}" );

    TRACE( "sending XSTS request\n" );
    hr = HttpRequest(
        L"POST",
        L"xsts.auth.xboxlive.com",
        L"/xsts/authorize",
        data,
        L"content-type: application/json",
        accept,
        &buffer,
        &size
    );

    free( data );
    if (FAILED( hr ))
    {
        WARN( "XSTS HttpRequest failed: 0x%08lx\n", hr );
        return hr;
    }

    TRACE( "XSTS response size=%llu, first 200 chars: %.200s\n", (unsigned long long)size, buffer );

    hr = ParseJsonObject( buffer, size, &object );
    free( buffer );
    if (FAILED( hr ))
    {
        WARN( "XSTS JSON parse failed: 0x%08lx\n", hr );
        return hr;
    }

    if (FAILED( hr = GetJsonStringValue( object, L"Token", token ) ))
    {
        IJsonObject_Release( object );
        return hr;
    }

    hr = GetJsonObjectValue( object, L"DisplayClaims", &display_claims );
    IJsonObject_Release( object );
    if (FAILED( hr ))
    {
        WindowsDeleteString( *token );
        return hr;
    }

    hr = GetJsonArrayValue( display_claims, L"xui", &xui );
    IJsonObject_Release( display_claims );
    if (FAILED( hr ))
    {
        WindowsDeleteString( *token );
        return hr;
    }

    hr = IJsonArray_GetObjectAt( xui, 0, &object );
    IJsonArray_Release( xui );
    if (FAILED( hr ))
    {
        WindowsDeleteString( *token );
        return hr;
    }

    if (FAILED( hr = GetJsonStringValue( object, L"agg", &agg )))
    {
        IJsonObject_Release( object );
        WindowsDeleteString( *token );
        return hr;
    }

    agg_str = WindowsGetStringRawBuffer( agg, &agg_len );
    if (agg_len >= 5 && wcsncmp( agg_str, L"Child", 5 )) *age_group = XUserAgeGroup_Child;
    else if (agg_len >= 4 && wcsncmp( agg_str, L"Teen", 4 )) *age_group = XUserAgeGroup_Teen;
    else if (agg_len >= 5 && wcsncmp( agg_str, L"Adult", 5 )) *age_group = XUserAgeGroup_Adult;
    else *age_group = XUserAgeGroup_Unknown;

    /* Extract gamertag if available */
    if (gamertag && gamertag_size > 0)
    {
        if (SUCCEEDED( GetJsonStringValue( object, L"gtg", &gtg ) ))
        {
            UINT32 gtg_len;
            LPSTR gtg_str;
            if (SUCCEEDED( HSTRINGToMultiByte( gtg, &gtg_str, &gtg_len ) ))
            {
                SIZE_T copy_len = gtg_len < gamertag_size - 1 ? gtg_len : gamertag_size - 1;
                memcpy( gamertag, gtg_str, copy_len );
                gamertag[copy_len] = '\0';
                free( gtg_str );
            }
            WindowsDeleteString( gtg );
        }
        else
        {
            gamertag[0] = '\0';
        }
    }

    hr = GetJsonStringValue( object, L"xid", &xid );
    IJsonObject_Release( object );
    if (FAILED( hr ))
    {
        WindowsDeleteString( *token );
        return hr;
    }

    hr = HSTRINGToMultiByte( xid, &xid_str, &xid_str_len );
    WindowsDeleteString( xid );
    if (FAILED( hr ))
    {
        WindowsDeleteString( *token );
        return hr;
    }

    *xuid = strtoull( xid_str, NULL, 10 );
    free( xid_str );
    if (errno == ERANGE)
    {
        WindowsDeleteString( *token );
        errno = 0;
        return E_FAIL;
    }

    return hr;
}

HRESULT RequestXstsTokenForRelyingParty( HSTRING user_token, LPCSTR relying_party, HSTRING *token )
{
    LPCWSTR accept[] = {L"application/json", NULL};
    UINT32 token_str_len;
    IJsonObject *object;
    LPSTR token_str;
    LPSTR buffer;
    SIZE_T size;
    HRESULT hr;
    LPSTR data;
    SIZE_T data_len;

    TRACE( "requesting XSTS token for RP: %s\n", relying_party );

    if (FAILED( hr = HSTRINGToMultiByte( user_token, &token_str, &token_str_len ) ))
        return hr;

    data_len = strlen( "{\"RelyingParty\":\"" ) + strlen( relying_party ) +
               strlen( "\",\"TokenType\":\"JWT\",\"Properties\":{\"SandboxId\":\"RETAIL\",\"UserTokens\":[\"" ) +
               token_str_len + strlen( "\"]}}" ) + 1;

    if (!(data = calloc( 1, data_len )))
    {
        free( token_str );
        return E_OUTOFMEMORY;
    }

    strcpy( data, "{\"RelyingParty\":\"" );
    strcat( data, relying_party );
    strcat( data, "\",\"TokenType\":\"JWT\",\"Properties\":{\"SandboxId\":\"RETAIL\",\"UserTokens\":[\"" );
    strncat( data, token_str, token_str_len );
    free( token_str );
    strcat( data, "\"]}}" );

    hr = HttpRequest(
        L"POST",
        L"xsts.auth.xboxlive.com",
        L"/xsts/authorize",
        data,
        L"content-type: application/json",
        accept,
        &buffer,
        &size
    );

    free( data );
    if (FAILED( hr ))
    {
        WARN( "XSTS request for RP %s failed: 0x%08lx\n", relying_party, hr );
        return hr;
    }

    TRACE( "XSTS response for RP %s: size=%llu\n", relying_party, (unsigned long long)size );

    hr = ParseJsonObject( buffer, size, &object );
    free( buffer );
    if (FAILED( hr )) return hr;

    hr = GetJsonStringValue( object, L"Token", token );
    IJsonObject_Release( object );

    return hr;
}

HRESULT RequestSisuAuthorize( LPCSTR client_id, HSTRING oauth_token,
                              HSTRING device_token, LPCSTR relying_party,
                              HSTRING *xsts_token )
{
    LPCWSTR accept[] = {L"application/json", NULL};
    LPSTR oauth_str = NULL, device_str = NULL;
    LPSTR proof_key_json = NULL;
    LPSTR sig_header = NULL;
    LPSTR body = NULL, response = NULL;
    UINT32 oauth_len, device_len;
    SIZE_T response_size;
    IJsonObject *root = NULL, *auth = NULL;
    HRESULT hr;

    if (!relying_party || !xsts_token) return E_POINTER;
    *xsts_token = NULL;
    if (!DeviceAuth_IsInitialized())
    {
        WARN( "RequestSisuAuthorize: DeviceAuth not initialised\n" );
        return E_FAIL;
    }

    if (FAILED( hr = HSTRINGToMultiByte( oauth_token, &oauth_str, &oauth_len ) ))
        goto cleanup;
    if (FAILED( hr = HSTRINGToMultiByte( device_token, &device_str, &device_len ) ))
        goto cleanup;
    if (FAILED( hr = DeviceAuth_GetProofKeyJson( &proof_key_json ) ))
        goto cleanup;

    /* Build SISU body: title-bound XSTS in a single call.  AppId is the
     * Bedrock MSA client_id Microsoft has linked to the Minecraft title
     * id; passing it here is what makes PlayFab accept the resulting
     * AuthorizationToken without a separate title.auth (which always
     * returns 401 without Microsoft's title credentials). */
    {
        SIZE_T body_cap = oauth_len + device_len + strlen( proof_key_json )
                          + strlen( relying_party ) + strlen( client_id ) + 512;
        if (!(body = calloc( 1, body_cap )))
        {
            hr = E_OUTOFMEMORY;
            goto cleanup;
        }
        snprintf( body, body_cap,
                  "{\"AccessToken\":\"t=%s\","
                  "\"AppId\":\"%s\","
                  "\"deviceToken\":\"%s\","
                  "\"Sandbox\":\"RETAIL\","
                  "\"UseModernGamertag\":true,"
                  "\"SiteName\":\"user.auth.xboxlive.com\","
                  "\"RelyingParty\":\"%s\","
                  "\"ProofKey\":%s}",
                  oauth_str, client_id, device_str, relying_party,
                  proof_key_json );
    }

    /* Sign the SISU request — Microsoft rejects unsigned /authorize. */
    if (FAILED( hr = DeviceAuth_SignRequest( "POST", "/authorize", "",
                                              body, strlen( body ),
                                              &sig_header ) ))
    {
        WARN( "RequestSisuAuthorize: SignRequest failed 0x%08lx\n", hr );
        goto cleanup;
    }

    {
        WCHAR sig_w[256];
        WCHAR headers[512];
        MultiByteToWideChar( CP_UTF8, 0, sig_header, -1, sig_w, 256 );
        swprintf( headers, 512,
                  L"content-type: application/json\r\n"
                  L"Signature: %s\r\n"
                  L"x-xbl-contract-version: 1",
                  sig_w );
        TRACE( "RequestSisuAuthorize POST sisu.xboxlive.com/authorize, "
               "client_id=%s, rp=%s\n", client_id, relying_party );
        hr = HttpRequest( L"POST", L"sisu.xboxlive.com", L"/authorize",
                          body, headers, accept, &response, &response_size );
        if (FAILED( hr ))
        {
            WARN( "RequestSisuAuthorize: HTTP failed 0x%08lx\n", hr );
            goto cleanup;
        }
    }
    TRACE( "RequestSisuAuthorize response size=%llu\n", (unsigned long long)response_size );
    /* Dump the full response in WARN so it lands even without +gdkc tracing
     * — we need to see whether AuthorizationToken is actually in the body
     * or just TitleToken/UserToken (Microsoft sometimes returns the latter
     * with a 200 when the device or proof key isn't trusted for the title).
     * Logged in chunks because WINE's debug helpers truncate long strings. */
    {
        SIZE_T off;
        for (off = 0; off < response_size; off += 800)
        {
            SIZE_T n = response_size - off;
            if (n > 800) n = 800;
            WARN( "RequestSisuAuthorize response[%llu..%llu]: %.*s\n",
                  (unsigned long long)off,
                  (unsigned long long)(off + n),
                  (int)n, response + off );
        }
    }

    if (FAILED( hr = ParseJsonObject( response, response_size, &root ) ))
    {
        WARN( "RequestSisuAuthorize: ParseJsonObject failed 0x%08lx\n", hr );
        goto cleanup;
    }

    /* SISU returns { AuthorizationToken: { Token, DisplayClaims, ... }, ... } */
    if (FAILED( hr = GetJsonObjectValue( root, L"AuthorizationToken", &auth ) ))
    {
        WARN( "RequestSisuAuthorize: AuthorizationToken not found in SISU response 0x%08lx\n", hr );
        goto cleanup;
    }
    hr = GetJsonStringValue( auth, L"Token", xsts_token );
    if (FAILED( hr ))
        WARN( "RequestSisuAuthorize: AuthorizationToken.Token missing 0x%08lx\n", hr );

cleanup:
    if (auth) IJsonObject_Release( auth );
    if (root) IJsonObject_Release( root );
    free( response );
    free( sig_header );
    free( body );
    free( proof_key_json );
    free( device_str );
    free( oauth_str );
    return hr;
}