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

    /* +1 so the buffer is NUL-terminated: wstr_len excludes the terminator, so
     * the conversion fills exactly *str_len bytes. Callers run strtoull() on the
     * result (xuid, uhs); without the trailing NUL it reads into adjacent memory
     * and appends stray digits — e.g. xuid 2535458430309376 -> 25354584303093761,
     * which then faults Minecraft. calloc zeroes the extra byte. */
    if (!(*str = calloc( 1, *str_len + 1 ))) return E_OUTOFMEMORY;

    if (!(*str_len = WideCharToMultiByte( CP_UTF8, 0, wstr, wstr_len, *str, *str_len, NULL, NULL )))
    {
        free( *str );
        *str = NULL;
        return HRESULT_FROM_WIN32( GetLastError() );
    }

    return S_OK;
}

static HRESULT HttpRequestWinHttp( LPCWSTR method, LPCWSTR domain, LPCWSTR object, LPSTR data, LPCWSTR headers, LPCWSTR *accept, LPSTR *buffer, SIZE_T *bufferSize )
{
    HINTERNET connection = NULL;
    DWORD size = sizeof( DWORD );
    HINTERNET session = NULL;
    HINTERNET request = NULL;
    HRESULT hr = S_OK;
    DWORD status;

    /* Use the same User-Agent xal/imLinguin's Bedrock-PlayFab auth uses —
     * Xbox Live's *.auth.xboxlive.com edges silently TCP-RST the connection
     * after seeing the HTTP request body when the UA doesn't look like a
     * known XAL/XSAPI client (we got `sock_recv recv error 10054` against
     * device.auth.xboxlive.com with "WineGDK/1.0"). Matches the XAL Win32
     * SDK string so the edge accepts the POST and replies normally. */
    if (!(session = WinHttpOpen(
        L"XAL Xbox Live Game (Windows; SDK; 1.0.0.0)",
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

    if (SUCCEEDED( hr ) && status / 100 != 2)
    {
        WARN( "HttpRequest %S%S returned HTTP %lu\n", domain, object, status );
        hr = E_FAIL;
    }

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

/* ---- OpenSSL transport via the shipped XCurl.dll (libcurl) ---------------
 * Wine 11.1's secur32/GnuTLS cannot complete the TLS handshake to the Azure
 * Front Door auth edges (sisu.xboxlive.com, xsts.auth.xboxlive.com and the
 * PlayFab relying party): they fail mid-session with 0x80072F7D /0x80090304,
 * so the engine cannot mint/refresh tokens once the launcher's pre-auth set
 * ages out (~1h). The Minecraft-Services session then collapses — Realms, the
 * franchise signaling socket and the MSA sign-in state all break. The game
 * ships an OpenSSL-backed libcurl (XCurl.dll) that reaches those exact edges
 * (it is what carries PlayFab LoginWithXbox), so drive it directly here. Falls
 * back to WinHttp when the DLL can't be loaded so first launch never regresses. */
typedef void CURL;
typedef void CURLM;
struct curl_slist;
typedef CURL  *(*curl_easy_init_t)( void );
typedef int    (*curl_easy_setopt_t)( CURL *, int, ... );
typedef int    (*curl_easy_getinfo_t)( CURL *, int, ... );
typedef void   (*curl_easy_cleanup_t)( CURL * );
typedef CURLM *(*curl_multi_init_t)( void );
typedef int    (*curl_multi_add_handle_t)( CURLM *, CURL * );
typedef int    (*curl_multi_remove_handle_t)( CURLM *, CURL * );
typedef int    (*curl_multi_perform_t)( CURLM *, int * );
typedef int    (*curl_multi_poll_t)( CURLM *, void *, unsigned, int, int * );
typedef void   (*curl_multi_cleanup_t)( CURLM * );
typedef struct curl_slist *(*curl_slist_append_t)( struct curl_slist *, const char * );
typedef void   (*curl_slist_free_all_t)( struct curl_slist * );

static struct curl_api
{
    BOOL tried, ok;
    HMODULE mod;
    curl_easy_init_t easy_init;
    curl_easy_setopt_t easy_setopt;
    curl_easy_getinfo_t easy_getinfo;
    curl_easy_cleanup_t easy_cleanup;
    curl_multi_init_t multi_init;
    curl_multi_add_handle_t multi_add;
    curl_multi_remove_handle_t multi_remove;
    curl_multi_perform_t multi_perform;
    curl_multi_poll_t multi_poll;
    curl_multi_cleanup_t multi_cleanup;
    curl_slist_append_t slist_append;
    curl_slist_free_all_t slist_free;
} g_curl;

static BOOL load_curl( void )
{
    if (g_curl.tried) return g_curl.ok;
    g_curl.tried = TRUE;
    if (!(g_curl.mod = LoadLibraryA( "xcurl.dll" )))
    {
        WARN( "xcurl.dll not loadable — auth HTTP stays on WinHttp\n" );
        return FALSE;
    }
#define LD(f,n) (g_curl.f = (void *)GetProcAddress( g_curl.mod, n ))
    g_curl.ok = LD(easy_init,"curl_easy_init") && LD(easy_setopt,"curl_easy_setopt") &&
                LD(easy_getinfo,"curl_easy_getinfo") && LD(easy_cleanup,"curl_easy_cleanup") &&
                LD(multi_init,"curl_multi_init") && LD(multi_add,"curl_multi_add_handle") &&
                LD(multi_remove,"curl_multi_remove_handle") && LD(multi_perform,"curl_multi_perform") &&
                LD(multi_poll,"curl_multi_poll") && LD(multi_cleanup,"curl_multi_cleanup") &&
                LD(slist_append,"curl_slist_append") && LD(slist_free,"curl_slist_free_all");
#undef LD
    if (g_curl.ok) TRACE( "xcurl.dll OpenSSL transport ready\n" );
    else WARN( "xcurl.dll missing curl_* exports — auth HTTP stays on WinHttp\n" );
    return g_curl.ok;
}

struct curl_resp { LPSTR data; SIZE_T len; };

static SIZE_T curl_write_cb( char *ptr, SIZE_T size, SIZE_T nmemb, void *ud )
{
    struct curl_resp *r = ud;
    SIZE_T add = size * nmemb;
    LPSTR grown = realloc( r->data, r->len + add );
    if (!grown) return 0;
    r->data = grown;
    memcpy( r->data + r->len, ptr, add );
    r->len += add;
    return add;
}

static LPSTR wide_to_utf8( LPCWSTR w )
{
    int n;
    LPSTR s;
    if (!w) return NULL;
    if ((n = WideCharToMultiByte( CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL )) <= 0) return NULL;
    if (!(s = malloc( n ))) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, w, -1, s, n, NULL, NULL );
    return s;
}

/* libcurl ABI constants (stable across versions) */
#define BOL_CURLOPT_URL            10002
#define BOL_CURLOPT_WRITEDATA      10001
#define BOL_CURLOPT_WRITEFUNCTION  20011
#define BOL_CURLOPT_POSTFIELDS     10015
#define BOL_CURLOPT_POSTFIELDSIZE  60
#define BOL_CURLOPT_HTTPHEADER     10023
#define BOL_CURLOPT_USERAGENT      10018
#define BOL_CURLOPT_CUSTOMREQUEST  10036
#define BOL_CURLOPT_FOLLOWLOCATION 52
#define BOL_CURLOPT_TIMEOUT        13
#define BOL_CURLOPT_NOSIGNAL       99
#define BOL_CURLINFO_RESPONSE_CODE 2097154
#define BOL_CURL_UNAVAILABLE       ((HRESULT)0x8007007EL) /* ERROR_MOD_NOT_FOUND: let WinHttp try */

static HRESULT HttpRequestCurl( LPCWSTR method, LPCWSTR domain, LPCWSTR object, LPSTR data, LPCWSTR headers, LPCWSTR *accept, LPSTR *buffer, SIZE_T *bufferSize )
{
    struct curl_resp resp = { NULL, 0 };
    struct curl_slist *hdrs = NULL;
    LPSTR url = NULL, dom = NULL, obj = NULL, mth = NULL, hdr = NULL;
    CURL *easy = NULL;
    CURLM *multi = NULL;
    HRESULT hr = E_FAIL;
    long status = 0;
    int running = 1;
    SIZE_T url_len;

    *buffer = NULL;
    *bufferSize = 0;
    if (!load_curl()) return BOL_CURL_UNAVAILABLE;

    dom = wide_to_utf8( domain );
    obj = wide_to_utf8( object );
    mth = wide_to_utf8( method );
    if (!dom || !obj) { hr = E_OUTOFMEMORY; goto done; }

    url_len = strlen( "https://" ) + strlen( dom ) + strlen( obj ) + 1;
    if (!(url = malloc( url_len ))) { hr = E_OUTOFMEMORY; goto done; }
    strcpy( url, "https://" );
    strcat( url, dom );
    strcat( url, obj );

    /* headers may be a single CRLF-joined block (WinHttp style) */
    if (headers && *headers && (hdr = wide_to_utf8( headers )))
    {
        LPSTR p = hdr;
        while (p && *p)
        {
            LPSTR nl = strstr( p, "\r\n" );
            if (nl) *nl = '\0';
            if (*p) hdrs = g_curl.slist_append( hdrs, p );
            p = nl ? nl + 2 : NULL;
        }
    }
    if (accept)
    {
        int i;
        for (i = 0; accept[i]; i++)
        {
            LPSTR a = wide_to_utf8( accept[i] ), line;
            SIZE_T n;
            if (!a) continue;
            n = strlen( "Accept: " ) + strlen( a ) + 1;
            if ((line = malloc( n )))
            {
                strcpy( line, "Accept: " );
                strcat( line, a );
                hdrs = g_curl.slist_append( hdrs, line );
                free( line );
            }
            free( a );
        }
    }

    if (!(easy = g_curl.easy_init()) || !(multi = g_curl.multi_init())) { hr = E_FAIL; goto done; }
    g_curl.easy_setopt( easy, BOL_CURLOPT_URL, url );
    g_curl.easy_setopt( easy, BOL_CURLOPT_USERAGENT, "XAL Xbox Live Game (Windows; SDK; 1.0.0.0)" );
    g_curl.easy_setopt( easy, BOL_CURLOPT_WRITEFUNCTION, curl_write_cb );
    g_curl.easy_setopt( easy, BOL_CURLOPT_WRITEDATA, &resp );
    g_curl.easy_setopt( easy, BOL_CURLOPT_FOLLOWLOCATION, 1L );
    g_curl.easy_setopt( easy, BOL_CURLOPT_TIMEOUT, 30L );
    g_curl.easy_setopt( easy, BOL_CURLOPT_NOSIGNAL, 1L );
    if (hdrs) g_curl.easy_setopt( easy, BOL_CURLOPT_HTTPHEADER, hdrs );
    if (mth && strcmp( mth, "GET" )) g_curl.easy_setopt( easy, BOL_CURLOPT_CUSTOMREQUEST, mth );
    if (data)
    {
        g_curl.easy_setopt( easy, BOL_CURLOPT_POSTFIELDS, data );
        g_curl.easy_setopt( easy, BOL_CURLOPT_POSTFIELDSIZE, (long)strlen( data ) );
    }

    g_curl.multi_add( multi, easy );
    do
    {
        if (g_curl.multi_perform( multi, &running )) break;
        if (running) g_curl.multi_poll( multi, NULL, 0, 1000, NULL );
    }
    while (running);

    g_curl.easy_getinfo( easy, BOL_CURLINFO_RESPONSE_CODE, &status );
    if (status / 100 == 2 && resp.data)
    {
        *buffer = resp.data;
        *bufferSize = resp.len;
        resp.data = NULL;
        hr = S_OK;
    }
    else
    {
        WARN( "HttpRequestCurl %S%S -> HTTP %ld\n", domain, object, status );
        hr = status ? E_FAIL : BOL_CURL_UNAVAILABLE; /* 0 = transport failed → let WinHttp try */
    }

done:
    if (multi && easy) g_curl.multi_remove( multi, easy );
    if (easy) g_curl.easy_cleanup( easy );
    if (multi) g_curl.multi_cleanup( multi );
    if (hdrs) g_curl.slist_free( hdrs );
    free( resp.data );
    free( url );
    free( dom );
    free( obj );
    free( mth );
    free( hdr );
    return hr;
}

HRESULT HttpRequest( LPCWSTR method, LPCWSTR domain, LPCWSTR object, LPSTR data, LPCWSTR headers, LPCWSTR *accept, LPSTR *buffer, SIZE_T *bufferSize )
{
    HRESULT hr = HttpRequestCurl( method, domain, object, data, headers, accept, buffer, bufferSize );
    if (hr == BOL_CURL_UNAVAILABLE)
        hr = HttpRequestWinHttp( method, domain, object, data, headers, accept, buffer, bufferSize );
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

    TRACE( "XSTS response size=%llu\n", (unsigned long long)size );

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
                              HSTRING *xsts_token, UINT64 *uhs )
{
    LPCWSTR accept[] = {L"application/json", NULL};
    LPSTR oauth_str = NULL, device_str = NULL;
    LPSTR proof_key_json = NULL;
    LPSTR sig_header = NULL;
    LPSTR body = NULL, response = NULL;
    UINT32 oauth_len, device_len;
    SIZE_T response_size;
    IJsonObject *root = NULL, *auth = NULL;
    IJsonObject *display = NULL, *xui0 = NULL;
    IJsonArray *xui = NULL;
    HRESULT hr;

    if (!relying_party || !xsts_token) return E_POINTER;
    *xsts_token = NULL;
    if (uhs) *uhs = 0;
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
                  "\"OfferTermsAcceptance\":true,"
                  "\"AcceptOffers\":true,"
                  "\"ProofKey\":%s}",
                  oauth_str, client_id, device_str, relying_party,
                  proof_key_json );
    }

    /* Sign the SISU request — Microsoft rejects unsigned /authorize. */
    if (FAILED( hr = DeviceAuth_SignRequest( "POST", "/authorize", "",
                                              0, NULL, body, strlen( body ),
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
    {
        WARN( "RequestSisuAuthorize: AuthorizationToken.Token missing 0x%08lx\n", hr );
        goto cleanup;
    }

    /* Extract AuthorizationToken.DisplayClaims.xui[0].uhs — PlayFab
     * cross-checks the uhs in the XBL3.0 header (`XBL3.0 x=<uhs>;<token>`)
     * against the uhs claim baked into the JWT.  Using the user-only
     * RequestUserToken uhs caused a mismatch and PlayFab silently looped
     * the sign-in screen even though the title-bound XSTS itself was
     * valid. */
    if (uhs)
    {
        HSTRING uhs_str = NULL;
        LPSTR uhs_mb = NULL;
        UINT32 uhs_len;
        if (SUCCEEDED( GetJsonObjectValue( auth, L"DisplayClaims", &display ) ) &&
            SUCCEEDED( GetJsonArrayValue( display, L"xui", &xui ) ) &&
            SUCCEEDED( IJsonArray_GetObjectAt( xui, 0, &xui0 ) ) &&
            SUCCEEDED( GetJsonStringValue( xui0, L"uhs", &uhs_str ) ) &&
            SUCCEEDED( HSTRINGToMultiByte( uhs_str, &uhs_mb, &uhs_len ) ))
        {
            errno = 0;
            *uhs = strtoull( uhs_mb, NULL, 10 );
            if (errno == ERANGE) { *uhs = 0; errno = 0; }
            TRACE( "RequestSisuAuthorize: AuthorizationToken user hash extracted\n" );
        }
        else WARN( "RequestSisuAuthorize: could not extract AuthorizationToken uhs\n" );
        if (uhs_mb) free( uhs_mb );
        if (uhs_str) WindowsDeleteString( uhs_str );
    }

cleanup:
    if (xui0) IJsonObject_Release( xui0 );
    if (xui) IJsonArray_Release( xui );
    if (display) IJsonObject_Release( display );
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
