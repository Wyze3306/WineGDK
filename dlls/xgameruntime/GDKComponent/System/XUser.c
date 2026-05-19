/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XUser
 *
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

#include "../../private.h"

#include <errno.h>
#include <time.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

const char *msaAppId = "0000000040159362";

static const WCHAR *ACCEPT_JSON[] = { L"application/json", NULL };
static const WCHAR *ACCEPT_PNG[] = { L"image/png", NULL };
static const WCHAR *CT_JSON = L"Content-Type: application/json";
static const WCHAR *CT_FORM_URLENCODED = L"Content-Type: application/x-www-form-urlencoded";

static HRESULT HttpRequest( const WCHAR *method, const WCHAR *url, char *data, const WCHAR *headers, const WCHAR **accept, void **buffer, SIZE_T *used )
{
    URL_COMPONENTS uc = { .dwStructSize = sizeof(URL_COMPONENTS), .dwHostNameLength = -1, .dwUrlPathLength = -1, .dwExtraInfoLength = -1};
    HINTERNET connection = NULL, request = NULL, session = NULL;
    WCHAR *hostName = NULL, *object = NULL;
    DWORD size = sizeof( DWORD ), status;
    char *temp_buffer = NULL;
    SIZE_T temp_used = 0;
    HRESULT hr = S_OK;

    TRACE( "method %s, url %s, data %s, headers %s, accept %p, buffer %p, used %p.\n", debugstr_w( method ), debugstr_w( url ), debugstr_a( data ), debugstr_w( headers ), accept, buffer, used );

    if (!WinHttpCrackUrl( url, wcslen( url ), 0, &uc )) return HRESULT_FROM_WIN32( GetLastError() );

    if (!(hostName = calloc( uc.dwHostNameLength + 1, sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    memcpy( hostName, uc.lpszHostName, uc.dwHostNameLength * sizeof(WCHAR) );

    if (!(object = calloc( uc.dwUrlPathLength + uc.dwExtraInfoLength + 1, sizeof(WCHAR) )))
    {
        hr = E_OUTOFMEMORY;
        goto _CLEANUP;
    }

    memcpy( object, uc.lpszUrlPath, uc.dwUrlPathLength * sizeof(WCHAR) );
    wcsncat( object, uc.lpszExtraInfo, uc.dwExtraInfoLength * sizeof(WCHAR) );

    if (!(session = WinHttpOpen( L"curl/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 ))) goto _FAILED;
    if (!(connection = WinHttpConnect( session, hostName, INTERNET_DEFAULT_HTTPS_PORT, 0 ))) goto _FAILED;
    if (!(request = WinHttpOpenRequest( connection, method, object, NULL, WINHTTP_NO_REFERER, accept, WINHTTP_FLAG_SECURE ))) goto _FAILED;
    if (!WinHttpSendRequest( request, headers, -1, data, strlen( data ), strlen( data ), 0 )) goto _FAILED;
    if (!WinHttpReceiveResponse( request, NULL )) goto _FAILED;
    if (!WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX )) goto _FAILED;
    TRACE( "%s %s -> HTTP %lu\n", debugstr_w( method ), debugstr_w( url ), status );
    if (status != 200)
    {
        /* Log why (Xbox puts a code in the x-err header / a JSON body) —
         * otherwise a rejected token is invisible and the game just loops. */
        WCHAR rawhdr[2048] = {0};
        char body[1024] = {0};
        DWORD hn = sizeof(rawhdr), got = 0, n = 0;

        WinHttpQueryHeaders( request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                             WINHTTP_HEADER_NAME_BY_INDEX, rawhdr, &hn,
                             WINHTTP_NO_HEADER_INDEX );
        while (got < sizeof(body) - 1 && WinHttpQueryDataAvailable( request, &n ) && n)
        {
            if (n > sizeof(body) - 1 - got) n = sizeof(body) - 1 - got;
            if (!WinHttpReadData( request, body + got, n, &n ) || !n) break;
            got += n;
        }
        ERR( "%s %s -> HTTP %lu\n  headers: %s\n  body: %s\n",
             debugstr_w( method ), debugstr_w( url ), status,
             debugstr_w( rawhdr ), debugstr_a( body ) );
        hr = E_FAIL;
        goto _CLEANUP;
    }

    /* buffer response data */
    do
    {
        if (!WinHttpQueryDataAvailable( request, &size )) goto _FAILED;
        if (!size) break;
        if (!(temp_buffer = realloc( temp_buffer, temp_used + size )))
        {
            hr = E_OUTOFMEMORY;
            goto _CLEANUP;
        }

        if (!WinHttpReadData( request, temp_buffer + temp_used, size, &size )) goto _FAILED;
        temp_used += size;
    }
    while (size);

    *buffer = temp_buffer;
    *used = temp_used;
    goto _CLEANUP;

_FAILED:

    hr = HRESULT_FROM_WIN32( GetLastError() );

_CLEANUP:

    if (object) free( object );
    if (hostName) free( hostName );
    if (request) WinHttpCloseHandle( request );
    if (session) WinHttpCloseHandle( session );
    if (connection) WinHttpCloseHandle( connection );
    if (SUCCEEDED(hr)) return hr;
    if (temp_buffer) free( temp_buffer );
    return hr;
}

static HRESULT MultiByteToHSTRING( const char *str, UINT32 str_size, HSTRING *hstr )
{
    UINT32 wstr_size;
    WCHAR *wstr;
    HRESULT hr;

    if (!(wstr_size = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, str, str_size, NULL, 0 )))
        return HRESULT_FROM_WIN32( GetLastError() );

    if (!(wstr = calloc( wstr_size, sizeof(WCHAR) ))) return E_OUTOFMEMORY;

    if (!(wstr_size = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, str, str_size, wstr, wstr_size )))
    {
        free( wstr );
        return HRESULT_FROM_WIN32( GetLastError() );
    }

    hr = WindowsCreateString( wstr, wstr_size, hstr );
    free( wstr );
    return hr;
}

static HRESULT HSTRINGToMultiByte( HSTRING hstr, char **str, UINT32 *str_size )
{
    UINT32 wstr_size;
    const WCHAR *wstr = WindowsGetStringRawBuffer( hstr, &wstr_size );

    if (!(*str_size = WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, wstr, wstr_size, NULL, 0, NULL, NULL )))
        return HRESULT_FROM_WIN32( GetLastError() );

    if (!(*str = calloc( 1, *str_size ))) return E_OUTOFMEMORY;

    if (!(*str_size = WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, wstr, wstr_size, *str, *str_size, NULL, NULL )))
    {
        free( *str );
        return HRESULT_FROM_WIN32( GetLastError() );
    }

    return S_OK;
}

#define GetJsonValue_( obj_type, ret_type )                                                                 \
static inline HRESULT GetJson##obj_type##Value( IJsonObject *object, const WCHAR *key, ret_type value )     \
{                                                                                                           \
    HSTRING_HEADER key_hdr;                                                                                 \
    HSTRING key_hstr;                                                                                       \
    HRESULT hr;                                                                                             \
                                                                                                            \
    if (FAILED(hr = WindowsCreateStringReference( key, wcslen( key ), &key_hdr, &key_hstr ))) return hr;    \
    return IJsonObject_GetNamed##obj_type( object, key_hstr, value );                                       \
}

GetJsonValue_( Object, IJsonObject** )
GetJsonValue_( Array, IJsonArray** )
GetJsonValue_( Value, IJsonValue** )
GetJsonValue_( String, HSTRING* )
GetJsonValue_( Number, DOUBLE* )

static HRESULT ParseJsonObject( const char *str, SIZE_T str_size, IJsonObject **object )
{
    const WCHAR *class_name = RuntimeClass_Windows_Data_Json_JsonValue;
    IJsonValueStatics *statics;
    HSTRING_HEADER class_hdr;
    HSTRING class, content;
    IJsonValue *value;
    HRESULT hr;

    if (FAILED(hr = WindowsCreateStringReference( class_name, wcslen( class_name ), &class_hdr, &class ))) return hr;
    if (FAILED(hr = RoGetActivationFactory( class, &IID_IJsonValueStatics, (void **)&statics ))) return hr;
    if (FAILED(hr = MultiByteToHSTRING( str, str_size, &content )))
    {
        IJsonValueStatics_Release( statics );
        return hr;
    }

    hr = IJsonValueStatics_Parse( statics, content, &value );
    IJsonValueStatics_Release( statics );
    WindowsDeleteString( content );
    if (FAILED(hr)) return hr;

    hr = IJsonValue_GetObject( value, object );
    IJsonValue_Release( value );
    return hr;
}

struct XUser
{
    IUser IUser_iface;
    LONG ref;

    UINT64 xuid;
    HSTRING user_hash;

    DOUBLE interval;
    time_t oauth_expiry;
    HSTRING device_code;
    HSTRING access_token;
    HSTRING refresh_token;
    HSTRING user_token;
    HSTRING xsts_token;

    char *classic_gamertag;
    UINT32 classic_gamertag_size;

    /* Xbox request-signing proof key (ECDSA P-256). The public key is sent
     * as "ProofKey" when the user token is minted, binding the token to
     * this key; every signed Xbox request is then signed with it. */
    BCRYPT_KEY_HANDLE proof_key;
    BYTE proof_x[32];
    BYTE proof_y[32];
};

static inline struct XUser *impl_from_IUser( IUser *iface )
{
    return CONTAINING_RECORD( iface, struct XUser, IUser_iface );
}

/* ------------------------------------------------------------------ *
 *  Xbox Live request signing
 *
 *  Xbox services require a "Signature" header on token requests and on
 *  service calls. The signature is ECDSA P-256 over a fixed byte layout
 *  (policy 0001 | FILETIME | method | path | Authorization | body), and
 *  the signing key must be bound to the user token by sending its public
 *  key as "ProofKey" in the user/authenticate request. Without this the
 *  token is unsigned and signed Xbox endpoints reject it. Layout matches
 *  the documented community scheme (gophertunnel minecraft/auth sign()).
 * ------------------------------------------------------------------ */

/* standard base64 of in[n], NUL-terminated, malloc'd (caller frees) */
static char *xbl_b64( const BYTE *in, DWORD n )
{
    DWORD len = 0;
    char *out;

    if (!CryptBinaryToStringA( in, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &len )) return NULL;
    if (!(out = calloc( 1, len + 1 ))) return NULL;
    if (!CryptBinaryToStringA( in, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &len ))
    {
        free( out );
        return NULL;
    }
    return out;
}

/* base64url (no padding) of in[n], malloc'd */
static char *xbl_b64url( const BYTE *in, DWORD n )
{
    char *s = xbl_b64( in, n ), *p;

    if (!s) return NULL;
    for (p = s; *p; p++)
    {
        if (*p == '+') *p = '-';
        else if (*p == '/') *p = '_';
        else if (*p == '=') { *p = 0; break; }
    }
    return s;
}

/* Generate (once) the ECDSA P-256 proof key and cache its public X/Y. */
static HRESULT xbl_ensure_key( struct XUser *impl )
{
    BYTE blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96];
    BCRYPT_ALG_HANDLE alg;
    NTSTATUS st;
    ULONG cb;

    if (impl->proof_key) return S_OK;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider( &alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0 )))
        return E_FAIL;
    st = BCryptGenerateKeyPair( alg, &impl->proof_key, 256, 0 );
    if (BCRYPT_SUCCESS(st)) st = BCryptFinalizeKeyPair( impl->proof_key, 0 );
    if (BCRYPT_SUCCESS(st)) st = BCryptExportKey( impl->proof_key, NULL, BCRYPT_ECCPUBLIC_BLOB,
                                                  blob, sizeof(blob), &cb, 0 );
    BCryptCloseAlgorithmProvider( alg, 0 );
    if (!BCRYPT_SUCCESS(st))
    {
        if (impl->proof_key) { BCryptDestroyKey( impl->proof_key ); impl->proof_key = NULL; }
        return E_FAIL;
    }

    /* BCRYPT_ECCKEY_BLOB header is followed by X then Y, cbKey bytes each
     * (32 for P-256), big-endian. */
    memcpy( impl->proof_x, blob + sizeof(BCRYPT_ECCKEY_BLOB), 32 );
    memcpy( impl->proof_y, blob + sizeof(BCRYPT_ECCKEY_BLOB) + 32, 32 );
    return S_OK;
}

/* "ProofKey":{...} JSON fragment (no leading comma), malloc'd. */
static char *xbl_proofkey_json( struct XUser *impl )
{
    char *x, *y, *out;
    const char *a = "\"ProofKey\":{\"crv\":\"P-256\",\"alg\":\"ES256\",\"use\":\"sig\",\"kty\":\"EC\",\"x\":\"";

    if (FAILED(xbl_ensure_key( impl ))) return NULL;
    if (!(x = xbl_b64url( impl->proof_x, 32 ))) return NULL;
    if (!(y = xbl_b64url( impl->proof_y, 32 ))) { free( x ); return NULL; }
    if ((out = calloc( 1, strlen( a ) + strlen( x ) + strlen( "\",\"y\":\"" ) + strlen( y ) + strlen( "\"}" ) + 1 )))
    {
        strcpy( out, a );
        strcat( out, x );
        strcat( out, "\",\"y\":\"" );
        strcat( out, y );
        strcat( out, "\"}" );
    }
    free( x );
    free( y );
    return out;
}

/* UTF-8 "/path?query" of a wide URL, malloc'd. */
static char *xbl_url_path( const WCHAR *url )
{
    URL_COMPONENTS uc = { .dwStructSize = sizeof(uc), .dwUrlPathLength = -1, .dwExtraInfoLength = -1 };
    int n;
    char *out;
    WCHAR *w;

    if (!WinHttpCrackUrl( url, wcslen( url ), 0, &uc )) return NULL;
    if (!(w = calloc( uc.dwUrlPathLength + uc.dwExtraInfoLength + 1, sizeof(WCHAR) ))) return NULL;
    memcpy( w, uc.lpszUrlPath, uc.dwUrlPathLength * sizeof(WCHAR) );
    if (uc.dwExtraInfoLength)
        memcpy( w + uc.dwUrlPathLength, uc.lpszExtraInfo, uc.dwExtraInfoLength * sizeof(WCHAR) );
    n = WideCharToMultiByte( CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL );
    if ((out = calloc( 1, n ? n : 1 )))
        WideCharToMultiByte( CP_UTF8, 0, w, -1, out, n, NULL, NULL );
    free( w );
    return out;
}

static void xbl_be64( BYTE *p, ULONGLONG v )
{
    for (int i = 7; i >= 0; i--) { p[i] = v & 0xff; v >>= 8; }
}

/* Build the Xbox "Signature" header value for one request. method/path/
 * authorization are NUL-terminated UTF-8; body may be NULL. malloc'd. */
static char *xbl_sign( struct XUser *impl, const char *method, const char *path,
                       const char *authorization, const BYTE *body, SIZE_T body_len )
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    BYTE digest[32], sig[64], hdr[76], z = 0, pol5[5] = {0,0,0,1,0}, ts8[8];
    NTSTATUS st;
    char *out = NULL;
    FILETIME ft;
    ULONGLONG ts;
    ULONG cb;

    if (!authorization) authorization = "";
    if (FAILED(xbl_ensure_key( impl ))) return NULL;

    GetSystemTimeAsFileTime( &ft );
    ts = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;  /* 100ns since 1601 */
    xbl_be64( ts8, ts );

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider( &alg, BCRYPT_SHA256_ALGORITHM, NULL, 0 ))) return NULL;
    if (!BCRYPT_SUCCESS(BCryptCreateHash( alg, &hash, NULL, 0, NULL, 0, 0 ))) goto done;

#define H(p,n) do { if (!BCRYPT_SUCCESS(BCryptHashData( hash, (PUCHAR)(p), (ULONG)(n), 0 ))) goto done; } while (0)
    H( pol5, 5 );                                   /* policy 0,0,0,1 + 0 */
    H( ts8, 8 ); H( &z, 1 );                        /* timestamp + 0 */
    H( method, strlen( method ) ); H( &z, 1 );      /* method + 0 */
    H( path, strlen( path ) ); H( &z, 1 );          /* path?query + 0 */
    H( authorization, strlen( authorization ) ); H( &z, 1 );
    if (body && body_len) H( body, body_len );
    H( &z, 1 );                                     /* body + 0 */
#undef H

    if (!BCRYPT_SUCCESS(BCryptFinishHash( hash, digest, sizeof(digest), 0 ))) goto done;
    st = BCryptSignHash( impl->proof_key, NULL, digest, sizeof(digest), sig, sizeof(sig), &cb, 0 );
    if (!BCRYPT_SUCCESS(st) || cb != sizeof(sig)) goto done;

    /* header = policy(0,0,0,1) + timestamp(8) + r||s(64) -> base64 */
    hdr[0] = 0; hdr[1] = 0; hdr[2] = 0; hdr[3] = 1;
    memcpy( hdr + 4, ts8, 8 );
    memcpy( hdr + 12, sig, 64 );
    out = xbl_b64( hdr, sizeof(hdr) );

done:
    if (hash) BCryptDestroyHash( hash );
    if (alg) BCryptCloseAlgorithmProvider( alg, 0 );
    return out;
}

/* Compose the headers string for a signed Xbox token request:
 * "Content-Type: application/json\r\nSignature: <sig>". malloc'd WCHAR. */
static WCHAR *xbl_signed_headers( struct XUser *impl, const WCHAR *url, const char *body )
{
    char *path, *sig = NULL;
    WCHAR *out = NULL;
    int n;

    if (!(path = xbl_url_path( url ))) return NULL;
    sig = xbl_sign( impl, "POST", path, "", (const BYTE *)body, body ? strlen( body ) : 0 );
    free( path );
    if (!sig) return NULL;

    n = MultiByteToWideChar( CP_UTF8, 0, sig, -1, NULL, 0 );
    if ((out = calloc( wcslen( L"Content-Type: application/json\r\nSignature: " ) + n + 1, sizeof(WCHAR) )))
    {
        wcscpy( out, L"Content-Type: application/json\r\nSignature: " );
        MultiByteToWideChar( CP_UTF8, 0, sig, -1, out + wcslen( out ), n );
    }
    free( sig );
    return out;
}

static HRESULT WINAPI user_QueryInterface( IUser *iface, REFIID iid, void **out )
{
    struct XUser *impl = impl_from_IUser( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IUser    ))
    {
        IUser_AddRef( *out = &impl->IUser_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI user_AddRef( IUser *iface )
{
    struct XUser *impl = impl_from_IUser( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI user_Release( IUser *iface )
{
    struct XUser *impl = impl_from_IUser( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    if (!ref)
    {
        if (impl->user_hash) WindowsDeleteString( impl->user_hash );
        if (impl->device_code) WindowsDeleteString( impl->device_code );
        if (impl->access_token) WindowsDeleteString( impl->access_token );
        if (impl->refresh_token) WindowsDeleteString( impl->refresh_token );
        if (impl->user_token) WindowsDeleteString( impl->user_token );
        if (impl->xsts_token) WindowsDeleteString( impl->xsts_token );
        if (impl->classic_gamertag) free( impl->classic_gamertag );
        if (impl->proof_key) BCryptDestroyKey( impl->proof_key );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI user_RequestOAuthCode( IUser *iface, HSTRING *user, HSTRING *uri )
{
    const char *template = "scope=service::user.auth.xboxlive.com::MBI_SSL&response_type=device_code&client_id=";
    struct XUser *impl = impl_from_IUser( iface );
    IJsonObject *object = NULL;
    void *buffer = NULL;
    SIZE_T size = 0;
    HRESULT hr;
    char *data;

    TRACE( "iface %p, user %p, uri %p.\n", iface, user, uri );

    data = calloc( strlen( template ) + strlen( msaAppId ) + 1, sizeof(char) );
    strcpy( data, template );
    strcat( data, msaAppId );

    hr = HttpRequest( L"POST", L"https://login.live.com/oauth20_connect.srf", data, CT_FORM_URLENCODED, ACCEPT_JSON, &buffer, &size );
    free( data );
    if (FAILED(hr)) return hr;

    hr = ParseJsonObject( buffer, size, &object );
    free( buffer );
    if (FAILED(hr)) return hr;

    impl->device_code = NULL;
    *user = NULL;
    *uri = NULL;

    if (FAILED(hr = GetJsonStringValue( object, L"device_code", &impl->device_code ))) goto _CLEANUP;
    if (FAILED(hr = GetJsonStringValue( object, L"verification_uri", uri ))) goto _CLEANUP;
    if (FAILED(hr = GetJsonNumberValue( object, L"interval", &impl->interval ))) goto _CLEANUP;
    hr = GetJsonStringValue( object, L"user_code", user );

_CLEANUP:

    IJsonObject_Release( object );
    if (SUCCEEDED(hr)) return hr;
    if (*uri) WindowsDeleteString( *uri );
    if (*user) WindowsDeleteString( *user );
    if (impl->device_code) WindowsDeleteString( impl->device_code );
    return hr;
}

static HRESULT WINAPI user_RequestOAuthToken( IUser *iface )
{
    const char *template = "grant_type=device_code&device_code=";
    HSTRING new_access = NULL, new_refresh = NULL;
    struct XUser *impl = impl_from_IUser( iface );
    char *data, *device_code = NULL;
    UINT32 device_code_size = 0;
    void *buffer = NULL;
    IJsonObject *object;
    DOUBLE delta = 0;
    SIZE_T size = 0;
    time_t expiry;
    HRESULT hr;

    TRACE( "iface %p.\n", iface );

    if (FAILED(hr = HSTRINGToMultiByte( impl->device_code, &device_code, &device_code_size ))) return hr;

    if (!(data = calloc( strlen( template ) + device_code_size + strlen( "&client_id=" ) + strlen( msaAppId ) + 1, sizeof(char) )))
    {
        free( device_code );
        return E_OUTOFMEMORY;
    }

    strcpy( data, template );
    strncat( data, device_code, device_code_size );
    strcat( data, "&client_id=" );
    strcat( data, msaAppId );
    free( device_code );

    while (TRUE)
    {
        if (SUCCEEDED(hr = HttpRequest( L"POST", L"https://login.live.com/oauth20_token.srf", data, CT_FORM_URLENCODED, ACCEPT_JSON, &buffer, &size ))) break;
        Sleep( impl->interval * 1000 );
    }

    free( data );
    hr = ParseJsonObject( buffer, size, &object );
    free( buffer );
    if (FAILED(hr)) return hr;

    if (FAILED(hr = GetJsonStringValue( object, L"refresh_token", &new_refresh ))) goto _CLEANUP;
    if (FAILED(hr = GetJsonStringValue( object, L"access_token", &new_access ))) goto _CLEANUP;
    if (FAILED(hr = GetJsonNumberValue( object, L"expires_in", &delta ))) goto _CLEANUP;

    if ((expiry = time(NULL)) == -1) hr = E_FAIL;
    else impl->oauth_expiry = expiry + delta;

_CLEANUP:

    IJsonObject_Release( object );
    if (SUCCEEDED(hr))
    {
        impl->refresh_token = new_refresh;
        impl->access_token = new_access;
        return hr;
    }

    if (new_refresh) WindowsDeleteString( new_refresh );
    if (new_access) WindowsDeleteString( new_access );
    return hr;
}

static HRESULT WINAPI user_RequestXToken( IUser *iface, const WCHAR *url, const char *relyingParty, const char *props, IUnknown **object )
{
    const char *template = "{\"TokenType\":\"JWT\",\"RelyingParty\":\"";
    WCHAR *signed_headers;
    void *buffer = NULL;
    SIZE_T size;
    HRESULT hr;
    char *data;

    TRACE( "iface %p, url %s, relyingParty %s, props %s, object %p.\n", iface, debugstr_w( url ), debugstr_a( relyingParty ), debugstr_a( props ), object );

    if (!(data = calloc(
        strlen( template ) + strlen( relyingParty ) + strlen( "\",\"Properties\":}" ) + strlen( props ) + 1, sizeof(char)
    ))) return E_OUTOFMEMORY;

    strcpy( data, template );
    strcat( data, relyingParty );
    strcat( data, "\",\"Properties\":" );
    strcat( data, props );
    strcat( data, "}" );

    /* user/authenticate and xsts/authorize must carry a Signature header
     * made with the proof key, or the resulting token is unsigned. */
    signed_headers = xbl_signed_headers( impl_from_IUser( iface ), url, data );
    hr = HttpRequest( L"POST", url, data, signed_headers ? signed_headers : CT_JSON,
                      ACCEPT_JSON, &buffer, &size );
    free( data );
    if (signed_headers) free( signed_headers );
    if (FAILED(hr)) return hr;

    hr = ParseJsonObject( buffer, size, (IJsonObject **)object );
    free( buffer );
    return hr;
}

static HRESULT WINAPI user_RefreshOAuthToken( IUser *iface )
{
    const char *template = "grant_type=refresh_token&scope=service::user.auth.xboxlive.com::MBI_SSL&client_id=";
    HSTRING new_access = NULL, new_refresh = NULL;
    struct XUser *impl = impl_from_IUser( iface );
    char *data, *refresh_str = NULL;
    IJsonObject *object = NULL;
    UINT32 refresh_size = 0;
    void *buffer = NULL;
    DOUBLE delta = 0;
    SIZE_T size = 0;
    time_t expiry;
    HRESULT hr;

    TRACE( "iface %p.\n", iface );

    if (FAILED(hr = HSTRINGToMultiByte( impl->refresh_token, &refresh_str, &refresh_size ))) return hr;

    if (!(data = calloc(
        strlen( template ) + strlen( msaAppId ) + strlen( "&refresh_token=" ) + refresh_size + 1, sizeof(char)
    ))) return E_OUTOFMEMORY;

    strcpy( data, template );
    strcat( data, msaAppId );
    strcat( data, "&refresh_token=" );
    strncat( data, refresh_str, refresh_size );

    hr = HttpRequest( L"POST", L"https://login.live.com/oauth20_token.srf", data, CT_FORM_URLENCODED, ACCEPT_JSON, &buffer, &size );
    free(data);
    if (FAILED(hr)) return hr;

    hr = ParseJsonObject( buffer, size, &object );
    free( buffer );
    if (FAILED(hr)) return hr;

    if (FAILED(hr = GetJsonStringValue( object, L"refresh_token", &new_refresh ))) goto _CLEANUP;
    if (FAILED(hr = GetJsonStringValue( object, L"access_token", &new_access ))) goto _CLEANUP;
    if (FAILED(hr = GetJsonNumberValue( object, L"expires_in", &delta ))) goto _CLEANUP;

    if ((expiry = time(NULL)) == -1) hr = E_FAIL;
    else impl->oauth_expiry = expiry + delta;

_CLEANUP:

    IJsonObject_Release( object );
    if (SUCCEEDED(hr))
    {
        impl->refresh_token = new_refresh;
        impl->access_token = new_access;
        return hr;
    }

    if (new_refresh) WindowsDeleteString( new_refresh );
    if (new_access) WindowsDeleteString( new_access );
    return hr;
}

static HRESULT WINAPI user_RefreshUserToken( IUser *iface )
{
    const char *template = "{\"AuthMethod\":\"RPS\",\"SiteName\":\"user.auth.xboxlive.com\",\"RpsTicket\":\"";
    struct XUser *impl = impl_from_IUser( iface );
    char *props, *token_str, *pk;
    IJsonObject *object;
    UINT32 token_size;
    HRESULT hr;

    TRACE( "iface %p.\n", iface );

    /* Bind the user token to the proof key: include its public key as
     * "ProofKey" here (the request itself is signed in RequestXToken). */
    if (!(pk = xbl_proofkey_json( impl ))) return E_FAIL;
    if (FAILED(hr = HSTRINGToMultiByte( impl->access_token, &token_str, &token_size )))
    {
        free( pk );
        return hr;
    }
    if (!(props = calloc( strlen( template ) + token_size + strlen( "\"," ) + strlen( pk ) + strlen( "}" ) + 1, sizeof(char) )))
    {
        free( token_str );
        free( pk );
        return E_OUTOFMEMORY;
    }

    strcpy( props, template );
    strncat( props, token_str, token_size );
    strcat( props, "\"," );
    strcat( props, pk );
    strcat( props, "}" );
    free( token_str );
    free( pk );

    hr = IUser_RequestXToken( iface, L"https://user.auth.xboxlive.com/user/authenticate", "http://auth.xboxlive.com", props, (IUnknown **)&object );
    free( props );
    if (FAILED(hr)) return hr;

    hr = GetJsonStringValue( object, L"Token", &impl->user_token );
    IJsonObject_Release( object );
    return hr;
}

static HRESULT WINAPI user_RefreshXstsToken( IUser *iface, const char *relyingParty )
{
    const char *template = "{\"SandboxId\":\"RETAIL\",\"UserTokens\":[\"";
    IJsonObject *child = NULL, *claims = NULL, *object;
    struct XUser *impl = impl_from_IUser( iface );
    IJsonArray *array = NULL;
    char *props, *token_str;
    HSTRING xuid = NULL;
    UINT32 token_size;
    HRESULT hr;

    TRACE( "iface %p, relyingParty %s.\n", iface, debugstr_a( relyingParty ) );

    if (FAILED(hr = HSTRINGToMultiByte( impl->user_token, &token_str, &token_size ))) return hr;
    if (!(props = calloc( strlen( template ) + token_size + strlen( "\"]}" ), sizeof(char) )))
    {
        free( token_str );
        return E_OUTOFMEMORY;
    }

    strcpy( props, template );
    strncat( props, token_str, token_size );
    strcat( props, "\"]}" );
    free( token_str );

    hr = IUser_RequestXToken( iface, L"https://xsts.auth.xboxlive.com/xsts/authorize", relyingParty, props, (IUnknown **)&object );
    free( props );
    if (FAILED(hr)) return hr;

    if (FAILED(hr = GetJsonStringValue( object, L"Token", &impl->xsts_token ))) goto _CLEANUP;
    if (FAILED(hr = GetJsonObjectValue( object, L"DisplayClaims", &claims ))) goto _CLEANUP;
    if (FAILED(hr = GetJsonArrayValue( claims, L"xui", &array ))) goto _CLEANUP;
    if (FAILED(hr = IJsonArray_GetObjectAt( array, 0, &child ))) goto _CLEANUP;
    if (FAILED(hr = GetJsonStringValue( child, L"uhs", &impl->user_hash ))) goto _CLEANUP;
    if (FAILED(hr = GetJsonStringValue( child, L"xid", &xuid ))) goto _CLEANUP;
    impl->xuid = wcstoull( WindowsGetStringRawBuffer( xuid, NULL ), NULL, 10 );
    if (errno == ERANGE) hr = E_UNEXPECTED;

_CLEANUP:

    IJsonObject_Release( object );
    if (xuid) WindowsDeleteString( xuid );
    if (array) IJsonArray_Release( array );
    if (child) IJsonObject_Release( child );
    if (claims) IJsonObject_Release( claims );
    return hr;
}

static HRESULT WINAPI user_FetchProfileSettings( IUser *iface, const WCHAR *settings, IUnknown **result )
{
    const WCHAR *template = L"https://profile.xboxlive.com/users/me/profile/settings?settings=";
    UINT32 headers_len, token_len, settings_count, uhs_len, url_len = wcslen( template ) + wcslen( settings ) + 1;
    const WCHAR *class_name = RuntimeClass_Windows_Data_Json_JsonObject;
    XUserHandle impl = impl_from_IUser( iface );
    WCHAR *data = NULL, *url, *headers;
    IJsonObject *object = NULL;
    IVector_IJsonValue *vector;
    const WCHAR *token, *uhs;
    HSTRING_HEADER class_hdr;
    HSTRING class, id = NULL;
    IJsonArray *array;
    IJsonValue *value;
    void *buffer;
    SIZE_T size;
    HRESULT hr;

    TRACE( "iface %p.\n", iface );

    *result = NULL;

    uhs = WindowsGetStringRawBuffer( impl->user_hash, &uhs_len );
    token = WindowsGetStringRawBuffer( impl->xsts_token, &token_len );

    headers_len = wcslen( L"x-xbl-contract-version: 2\r\nAuthorization: XBL3.0 x=;" ) + uhs_len + token_len + 1;
    url_len = wcslen( template ) + wcslen( settings ) + 1;
    if (!(data = calloc( headers_len + url_len, sizeof(WCHAR) ))) return E_OUTOFMEMORY;

    headers = data;
    wcscpy( headers, L"x-xbl-contract-version: 2\r\nAuthorization: XBL3.0 x=" );
    wcsncat( headers, uhs, uhs_len );
    wcscat( headers, L";" );
    wcsncat( headers, token, token_len );

    url = data + headers_len;
    wcscpy( url, template );
    wcscat( url, settings );

    hr = HttpRequest( L"GET", url, (char *)"", headers, ACCEPT_JSON, &buffer, &size );
    free( data );
    if (FAILED(hr)) return hr;

    hr = ParseJsonObject( buffer, size, &object );
    free( buffer );
    if (FAILED(hr)) return hr;

    hr = GetJsonArrayValue( object, L"profileUsers", &array );
    IJsonObject_Release( object );
    if (FAILED(hr)) return hr;

    hr = IJsonArray_GetObjectAt( array, 0, &object );
    IJsonArray_Release( array );
    if (FAILED(hr)) return hr;

    hr = GetJsonArrayValue( object, L"settings", &array );
    IJsonObject_Release( object );
    if (FAILED(hr)) return hr;

    /* repack settings for easier access */

    if (FAILED(hr = IJsonArray_QueryInterface( array, &IID_IVector_IJsonValue, (void **)&vector ))) goto _CLEANUP;
    hr = IVector_IJsonValue_get_Size( vector, &settings_count );
    IVector_IJsonValue_Release( vector );
    if (FAILED(hr)) goto _CLEANUP;

    if (FAILED(hr = WindowsCreateStringReference( class_name, wcslen( class_name ), &class_hdr, &class ))) goto _CLEANUP;
    if (FAILED(hr = RoActivateInstance( class, (IInspectable **)result ))) goto _CLEANUP;

    for (UINT32 i = 0; i < settings_count; i++)
    {
        if (FAILED(hr = IJsonArray_GetObjectAt( array, i, &object ))) goto _CLEANUP;
        if (FAILED(hr = GetJsonStringValue( object, L"id", &id ))) goto _CLEANUP;
        if (FAILED(hr = GetJsonValueValue( object, L"value", &value ))) goto _CLEANUP;
        IJsonObject_Release( object );
        object = NULL;
        hr = IJsonObject_SetNamedValue( (IJsonObject *)*result, id, value );
        IJsonValue_Release( value );
        if (FAILED(hr)) goto _CLEANUP;
        WindowsDeleteString( id );
        id = NULL;
    }

_CLEANUP:

    IJsonArray_Release( array );
    if (id) WindowsDeleteString( id );
    if (object) IJsonObject_Release( object );
    if (FAILED(hr) && *result)
    {
        IUnknown_Release( *result );
        *result = NULL;
    }
    return hr;
}

static const struct IUserVtbl user_vtbl =
{
    user_QueryInterface,
    user_AddRef,
    user_Release,
    /* IUser methods */
    user_RequestOAuthCode,
    user_RequestOAuthToken,
    user_RequestXToken,
    user_RefreshOAuthToken,
    user_RefreshUserToken,
    user_RefreshXstsToken,
    user_FetchProfileSettings,
};

static HRESULT LoadDefaultUser( XUserHandle *user )
{
    HSTRING classic_gamertag;
    IJsonObject *object;
    char *buffer = NULL;
    struct XUser *impl;
    LSTATUS status;
    IUser *iface;
    HRESULT hr;
    DWORD size;

    TRACE( "user %p.\n", user );

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;

    impl->IUser_iface.lpVtbl = &user_vtbl;
    impl->ref = 1;

    impl->user_hash = NULL;
    impl->device_code = NULL;
    impl->access_token = NULL;
    impl->refresh_token = NULL;
    impl->user_token = NULL;
    impl->xsts_token = NULL;

    iface = &impl->IUser_iface;

    if (ERROR_SUCCESS != (status = RegGetValueA(
        HKEY_LOCAL_MACHINE, "Software\\Wine\\WineGDK", "RefreshToken", RRF_RT_REG_SZ, NULL, NULL, &size
    )))
    {
        hr = HRESULT_FROM_WIN32( status );
        goto _CLEANUP;
    }

    if (!(buffer = calloc( 1, size )))
    {
        hr = E_OUTOFMEMORY;
        goto _CLEANUP;
    }

    if (ERROR_SUCCESS != (status = RegGetValueA(
        HKEY_LOCAL_MACHINE, "Software\\Wine\\WineGDK", "RefreshToken", RRF_RT_REG_SZ, NULL, buffer, &size
    )))
    {
        hr = HRESULT_FROM_WIN32( status );
        goto _CLEANUP;
    }

    if (FAILED(hr = MultiByteToHSTRING( buffer, size, &impl->refresh_token ))) goto _CLEANUP;
    if (FAILED(hr = IUser_RefreshOAuthToken( iface ))) goto _CLEANUP;
    if (FAILED(hr = IUser_RefreshUserToken( iface ))) goto _CLEANUP;
    if (FAILED(hr = IUser_RefreshXstsToken( iface, "http://xboxlive.com" ))) goto _CLEANUP;
    if (FAILED(hr = IUser_FetchProfileSettings( iface, L"Gamertag", (IUnknown **)&object ))) goto _CLEANUP;
    hr = GetJsonStringValue( object, L"Gamertag", &classic_gamertag );
    IJsonObject_Release( object );
    if (FAILED(hr)) goto _CLEANUP;
    hr = HSTRINGToMultiByte( classic_gamertag, &impl->classic_gamertag, &impl->classic_gamertag_size );
    WindowsDeleteString( classic_gamertag );

_CLEANUP:

    if (buffer) free( buffer );
    if (SUCCEEDED(hr)) *user = (XUserHandle)impl;
    else IUser_Release( iface );
    return hr;
}

struct x_user
{
    IXUserImpl6 IXUserImpl_iface;
    IXUserGamertagImpl IXUserGamertagImpl_iface;
    LONG ref;
};

static inline struct x_user *impl_from_IXUserImpl( IXUserImpl6 *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserImpl_iface );
}

static HRESULT WINAPI x_user_QueryInterface( IXUserImpl6 *iface, REFIID iid, void **out )
{
    struct x_user *impl = impl_from_IXUserImpl( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown    ) ||
        IsEqualGUID( iid, &IID_IXUserImpl  ) ||
        IsEqualGUID( iid, &IID_IXUserImpl2 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl3 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl4 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl5 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl6 ))
    {
        IXUserImpl6_AddRef( *out = &impl->IXUserImpl_iface );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IXUserGamertagImpl ))
    {
        IXUserGamertagImpl_AddRef( *out = &impl->IXUserGamertagImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_AddRef( IXUserImpl6 *iface )
{
    struct x_user *impl = impl_from_IXUserImpl( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_user_Release( IXUserImpl6 *iface )
{
    struct x_user *impl = impl_from_IXUserImpl( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_user_XUserDuplicateHandle( IXUserImpl6 *iface, XUserHandle handle, XUserHandle *duplicatedHandle )
{
    TRACE( "iface %p, handle %p, duplicatedHandle %p.\n", iface, handle, duplicatedHandle );
    if (!handle || !duplicatedHandle) return E_POINTER;
    IUser_AddRef( &handle->IUser_iface );
    *duplicatedHandle = handle;
    return S_OK;
}

static void WINAPI x_user_XUserCloseHandle( IXUserImpl6 *iface, XUserHandle user )
{
    TRACE( "iface %p, user %p.\n", iface, user );
    if (!user) return;
    IUser_Release( &user->IUser_iface );
}

static INT32 WINAPI x_user_XUserCompare( IXUserImpl6 *iface, XUserHandle user1, XUserHandle user2 )
{
    TRACE( "iface %p, user1 %p, user2 %p.\n", iface, user1, user2 );
    if (!user1 || !user2) return 1;
    return user1->xuid == user2->xuid ? 0 : 1;
}

static HRESULT WINAPI x_user_XUserGetMaxUsers( IXUserImpl6 *iface, UINT32 *maxUsers )
{
    TRACE( "iface %p, maxUsers %p.\n", iface, maxUsers );
    *maxUsers = 1;
    return S_OK;
}

struct XUserAddContext
{
    XUserAddOptions options;
    XUserHandle user;
};

static HRESULT WINAPI XUserAddProvider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct XUserAddContext *context;
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "op %d, data %p.\n", op, data );

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    context = (struct XUserAddContext *)data->context;

    switch (op)
    {
        case XAsyncOp_Begin:
            hr = IXThreadingImpl_XAsyncSchedule( xthreading, data->async, 0 );
            break;

        case XAsyncOp_GetResult:
            memcpy( data->buffer, &context->user, sizeof(XUserHandle) );
            break;

        case XAsyncOp_DoWork:
            if (context->options & XUserAddOptions_AddDefaultUserSilently)
                hr = LoadDefaultUser( &context->user );
            else hr = E_ABORT;

            IXThreadingImpl_XAsyncComplete( xthreading, data->async, hr, sizeof(XUserHandle) );
            hr = S_OK;
            break;

        case XAsyncOp_Cleanup:
            free( context );
            break;

        case XAsyncOp_Cancel:
            break;
    }

    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_user_XUserAddAsync( IXUserImpl6 *iface, XUserAddOptions options, XAsyncBlock *async )
{
    struct XUserAddContext *context;
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, options %d, async %p.\n", iface, options, async );

    if (!async) return E_POINTER;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    if (!(context = calloc( 1, sizeof(*context) )))
    {
        IXThreadingImpl_Release( xthreading );
        return E_OUTOFMEMORY;
    }

    context->options = options;
    hr = IXThreadingImpl_XAsyncBegin( xthreading, async, context, NULL, "XUserAddAsync", XUserAddProvider );
    IXThreadingImpl_Release( xthreading );
    if (FAILED(hr)) free( context );
    return hr;
}

static HRESULT WINAPI x_user_XUserAddResult( IXUserImpl6 *iface, XAsyncBlock *async, XUserHandle *newUser )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, async %p, newUser %p.\n", iface, async, newUser );

    if (!async || !newUser) return E_POINTER;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult( xthreading, async, NULL, sizeof(*newUser), newUser, NULL );
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetLocalId( IXUserImpl6 *iface, XUserHandle user, XUserLocalId *userLocalId )
{
    FIXME( "iface %p, user %p, userLocalId %p stub!\n", iface, user, userLocalId );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserFindUserByLocalId( IXUserImpl6 *iface, XUserLocalId userLocalId, XUserHandle *handle )
{
    FIXME( "iface %p, userLocalId %p, handle %p stub!\n", iface, &userLocalId, handle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetId( IXUserImpl6 *iface, XUserHandle user, UINT64 *userId )
{
    TRACE( "iface %p, user %p, userId %p.\n", iface, user, userId );
    if (!user || !userId) return E_POINTER;
    *userId = user->xuid;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserFindUserById( IXUserImpl6 *iface, UINT64 userId, XUserHandle *handle )
{
    FIXME( "iface %p, userId %llu, handle %p stub!\n", iface, userId, handle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetIsGuest( IXUserImpl6 *iface, XUserHandle user, BOOLEAN *isGuest )
{
    TRACE( "iface %p, user %p, isGuest %p.\n", iface, user, isGuest );
    if (!user || !isGuest) return E_POINTER;
    *isGuest = FALSE;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetState( IXUserImpl6 *iface, XUserHandle user, XUserState *state )
{
    TRACE( "iface %p, user %p, state %p.\n", iface, user, state );
    if (!user || !state) return E_POINTER;
    /* A handle with a user token completed OAuth->RPS->user-token, i.e.
     * it is signed in. The game polls this to drive its sign-in UI. */
    *state = user->user_token ? XUserState_SignedIn : XUserState_SignedOut;
    return S_OK;
}

static HRESULT WINAPI __PADDING__( IXUserImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

struct XUserGetGamerPictureContext
{
    XUserHandle user;
    XUserGamerPictureSize pictureSize;
    SIZE_T bufferSize;
    void *buffer;
};

static HRESULT WINAPI XUserGetGamerPictureProvider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct XUserGetGamerPictureContext *context;
    const WCHAR *buffer, *url_suffix;
    IXThreadingImpl *xthreading;
    IJsonObject *object;
    UINT32 buffer_len;
    WCHAR *full_url;
    HSTRING url;
    HRESULT hr;

    TRACE( "op %d, data %p.\n", op, data );

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    context = (struct XUserGetGamerPictureContext *)data->context;

    switch (op)
    {
        case XAsyncOp_Begin:
            hr = IXThreadingImpl_XAsyncSchedule( xthreading, data->async, 0 );
            break;

        case XAsyncOp_GetResult:
            memmove( data->buffer, context->buffer, context->bufferSize );
            break;

        case XAsyncOp_DoWork:
            switch (context->pictureSize)
            {
                case XUserGamerPictureSize_Small:
                    url_suffix = L"&format=png&w=64&h=64";
                    break;
                case XUserGamerPictureSize_Medium:
                    url_suffix = L"&format=png&w=208&h=208";
                    break;
                case XUserGamerPictureSize_Large:
                    url_suffix = L"&format=png&w=424&h=424";
                    break;
                case XUserGamerPictureSize_ExtraLarge:
                    url_suffix = L"&format=png&w=1080&h=1080";
                    break;
                default:
                    hr = E_INVALIDARG;
                    goto _CLEANUP;
            }

            if (FAILED(hr = IUser_FetchProfileSettings( &context->user->IUser_iface, L"PublicGamerpic", (IUnknown **)&object ))) goto _CLEANUP;
            hr = GetJsonStringValue( object, L"PublicGamerpic", &url );
            IJsonObject_Release( object );
            if (FAILED(hr)) goto _CLEANUP;

            buffer = WindowsGetStringRawBuffer( url, &buffer_len );
            if (!(full_url = calloc( buffer_len + wcslen( url_suffix ) + 1, sizeof(WCHAR) )))
            {
                hr = E_OUTOFMEMORY;
                goto _CLEANUP;
            }

            memcpy( full_url, buffer, buffer_len * sizeof(WCHAR) );
            wcscat( full_url, url_suffix );

            hr = HttpRequest( L"GET", full_url, NULL, NULL, ACCEPT_PNG, &context->buffer, &context->bufferSize );
            WindowsDeleteString( url );
            free( full_url );

        _CLEANUP:

            IXThreadingImpl_XAsyncComplete( xthreading, data->async, hr, context->bufferSize );
            hr = S_OK;
            break;

        case XAsyncOp_Cleanup:
            IXUserImpl6_XUserCloseHandle( x_user_impl, context->user );
            if (context->buffer) free( context->buffer );
            free( context );
            break;

        case XAsyncOp_Cancel:
            break;
    }

    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureAsync( IXUserImpl6 *iface, XUserHandle user, XUserGamerPictureSize pictureSize, XAsyncBlock *async )
{
    struct XUserGetGamerPictureContext *context;
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, user %p, pictureSize %d, async %p.\n", iface, user, pictureSize, async );

    if (!user || !async) return E_POINTER;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    if (!(context = calloc( 1, sizeof(*context) )))
    {
        IXThreadingImpl_Release( xthreading );
        return E_OUTOFMEMORY;
    }

    context->pictureSize = pictureSize;
    if (FAILED(hr = IXUserImpl6_XUserDuplicateHandle( iface, user, &context->user )))
    {
        IXThreadingImpl_Release( xthreading );
        return hr;
    }

    hr = IXThreadingImpl_XAsyncBegin( xthreading, async, context, NULL, "XUserGetGamerPictureAsync", XUserGetGamerPictureProvider );
    IXThreadingImpl_Release( xthreading );
    if (FAILED(hr)) free( context );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, async %p, bufferSize %p.\n", iface, async, bufferSize );

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResultSize( xthreading, async, bufferSize );
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, SIZE_T *bufferUsed )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, async %p, bufferSize %Iu, buffer %p, bufferUsed %p.\n", iface, async, bufferSize, buffer, bufferUsed );

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult( xthreading, async, NULL, bufferSize, buffer, bufferUsed );
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetAgeGroup( IXUserImpl6 *iface, XUserHandle user, XUserAgeGroup *ageGroup )
{
    TRACE( "iface %p, user %p, ageGroup %p.\n", iface, user, ageGroup );
    if (!user || !ageGroup) return E_POINTER;
    *ageGroup = XUserAgeGroup_Adult;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserCheckPrivilege( IXUserImpl6 *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, BOOLEAN *hasPrivilege, XUserPrivilegeDenyReason *reason )
{
    TRACE( "iface %p, user %p, options %d, privilege %d, hasPrivilege %p, reason %p.\n", iface, user, options, privilege, hasPrivilege, reason );
    if (!user || !hasPrivilege) return E_POINTER;
    /* Grant the privileges the game gates online/multiplayer on; the real
     * enforcement still happens server-side via the XSTS token. */
    *hasPrivilege = TRUE;
    if (reason) *reason = XUserPrivilegeDenyReason_None;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiAsync( IXUserImpl6 *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, options %d, privilege %d, async %p stub!\n", iface, user, options, privilege, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

struct XUserGetTokenAndSignatureContext
{
    XUserHandle user;
    XUserGetTokenAndSignatureOptions options;
    SIZE_T headerCount;
    SIZE_T bodySize;
    void *bodyBuffer;
    BOOLEAN isUtf16;
    union
    {
        struct
        {
            char *method;
            char *url;
            XUserGetTokenAndSignatureHttpHeader *headers;
            XUserGetTokenAndSignatureData *data;
        };
        struct
        {
            WCHAR *methodUtf16;
            WCHAR *urlUtf16;
            XUserGetTokenAndSignatureUtf16HttpHeader *headersUtf16;
            XUserGetTokenAndSignatureUtf16Data *dataUtf16;
        };
    };
};

static HRESULT WINAPI XUserGetTokenAndSignatureProvider( XAsyncOp op, const XAsyncProviderData *data )
{
    const char *template = "{\"SandboxId\":\"RETAIL\",\"UserTokens\":[\"";
    struct XUserGetTokenAndSignatureContext *context;
    char *props, *token_str = NULL;
    UINT32 data_size, token_size;
    IXThreadingImpl *xthreading;
    IJsonObject *object;
    HSTRING token;
    IUser *user;
    HRESULT hr;

    TRACE( "op %d, data %p.\n", op, data );

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    context = (struct XUserGetTokenAndSignatureContext *)data->context;
    user = &context->user->IUser_iface;

    switch (op)
    {
        case XAsyncOp_Begin:
            hr = IXThreadingImpl_XAsyncSchedule( xthreading, data->async, 0 );
            break;

        case XAsyncOp_GetResult:
            if (context->isUtf16)
                memcpy( data->buffer, context->dataUtf16, sizeof(*context->dataUtf16) +
                        (context->dataUtf16->tokenCount + context->dataUtf16->signatureCount) * sizeof(WCHAR) );
            else
                memcpy( data->buffer, context->data, sizeof(*context->data) +
                        context->data->tokenSize + context->data->signatureSize );
            break;

        case XAsyncOp_DoWork:
            if (FAILED(hr = HSTRINGToMultiByte( context->user->user_token, &token_str, &token_size ))) goto _CLEANUP;
            if (!(props = calloc( strlen( template ) + token_size + strlen( "\"]}" ), sizeof(char) )))
            {
                free( token_str );
                hr = E_OUTOFMEMORY;
                goto _CLEANUP;
            }

            strcpy( props, template );
            strncat( props, token_str, token_size );
            strcat( props, "\"]}" );
            free( token_str );

            hr = IUser_RequestXToken( user, L"https://xsts.auth.xboxlive.com/xsts/authorize", context->url, props, (IUnknown **)&object );
            free( props );
            if (FAILED(hr)) goto _CLEANUP;
            hr = GetJsonStringValue( object, L"Token", &token );
            IJsonObject_Release( object );
            if (FAILED(hr)) goto _CLEANUP;

            if (context->isUtf16)
            {
                UINT32 tok_len, uhs_len, auth_len, sigw_count = 0;
                const WCHAR *uhs = WindowsGetStringRawBuffer( context->user->user_hash, &uhs_len );
                const WCHAR *tok = WindowsGetStringRawBuffer( token, &tok_len );
                char *methodA = NULL, *pathA = NULL, *authA = NULL, *sigA;
                WCHAR *auth, *sigw = NULL, *dst;
                int wn;

                auth_len = wcslen( L"XBL3.0 x=;" ) + uhs_len + tok_len + 1;
                if (!(auth = calloc( auth_len, sizeof(WCHAR) )))
                {
                    WindowsDeleteString( token );
                    hr = E_OUTOFMEMORY;
                    goto _CLEANUP;
                }
                wcscpy( auth, L"XBL3.0 x=" );
                wcsncat( auth, uhs, uhs_len );
                wcscat( auth, L";" );
                wcsncat( auth, tok, tok_len );
                WindowsDeleteString( token );

                /* signature over the game's actual request */
                if ((wn = WideCharToMultiByte( CP_UTF8, 0, context->methodUtf16, -1, NULL, 0, NULL, NULL )) &&
                    (methodA = calloc( 1, wn )))
                    WideCharToMultiByte( CP_UTF8, 0, context->methodUtf16, -1, methodA, wn, NULL, NULL );
                pathA = xbl_url_path( context->urlUtf16 );
                if ((wn = WideCharToMultiByte( CP_UTF8, 0, auth, -1, NULL, 0, NULL, NULL )) &&
                    (authA = calloc( 1, wn )))
                    WideCharToMultiByte( CP_UTF8, 0, auth, -1, authA, wn, NULL, NULL );
                sigA = xbl_sign( context->user, methodA ? methodA : "GET", pathA ? pathA : "/",
                                 authA ? authA : "", context->bodyBuffer, context->bodySize );
                if (sigA && (wn = MultiByteToWideChar( CP_UTF8, 0, sigA, -1, NULL, 0 )) &&
                    (sigw = calloc( wn, sizeof(WCHAR) )))
                {
                    MultiByteToWideChar( CP_UTF8, 0, sigA, -1, sigw, wn );
                    sigw_count = wn;   /* incl. NUL, in WCHARs */
                }
                free( methodA ); free( pathA ); free( authA ); free( sigA );

                data_size = sizeof(*context->dataUtf16) + (auth_len + sigw_count) * sizeof(WCHAR);
                if (!(context->dataUtf16 = calloc( 1, data_size )))
                {
                    free( auth ); free( sigw );
                    hr = E_OUTOFMEMORY;
                    goto _CLEANUP;
                }

                context->dataUtf16->tokenCount = auth_len;
                context->dataUtf16->signatureCount = sigw_count;
                context->dataUtf16->token = NULL;
                context->dataUtf16->signature = NULL;

                dst = (WCHAR *)((char *)context->dataUtf16 + sizeof(*context->dataUtf16));
                memcpy( dst, auth, auth_len * sizeof(WCHAR) );
                if (sigw) memcpy( dst + auth_len, sigw, sigw_count * sizeof(WCHAR) );
                free( auth ); free( sigw );
            }
            else
            {
                UINT32 auth_len, tok_len, uhs_len, sig_size = 0;
                char *auth, *tok, *uhs, *sig, *pathA = NULL;
                WCHAR *wurl = NULL;
                int wn;

                hr = HSTRINGToMultiByte( token, &tok, &tok_len );
                WindowsDeleteString( token );
                if (FAILED(hr)) goto _CLEANUP;
                if (FAILED(hr = HSTRINGToMultiByte( context->user->user_hash, &uhs, &uhs_len )))
                {
                    free( tok );
                    goto _CLEANUP;
                }

                auth_len = strlen( "XBL3.0 x=;" ) + uhs_len + tok_len + 1;
                if (!(auth = calloc( 1, auth_len )))
                {
                    free( tok ); free( uhs );
                    hr = E_OUTOFMEMORY;
                    goto _CLEANUP;
                }
                strcpy( auth, "XBL3.0 x=" );
                strncat( auth, uhs, uhs_len );
                strcat( auth, ";" );
                strncat( auth, tok, tok_len );
                free( tok );
                free( uhs );

                /* signature over the game's actual request */
                if ((wn = MultiByteToWideChar( CP_UTF8, 0, context->url, -1, NULL, 0 )) &&
                    (wurl = calloc( wn, sizeof(WCHAR) )))
                {
                    MultiByteToWideChar( CP_UTF8, 0, context->url, -1, wurl, wn );
                    pathA = xbl_url_path( wurl );
                }
                sig = xbl_sign( context->user, context->method, pathA ? pathA : "/",
                                auth, context->bodyBuffer, context->bodySize );
                sig_size = sig ? strlen( sig ) + 1 : 0;

                data_size = sizeof(*context->data) + auth_len + sig_size;
                if (!(context->data = calloc( 1, data_size )))
                {
                    free( auth ); free( sig ); free( pathA ); free( wurl );
                    hr = E_OUTOFMEMORY;
                    goto _CLEANUP;
                }

                context->data->tokenSize = auth_len;
                context->data->signatureSize = sig_size;
                context->data->token = NULL;
                context->data->signature = NULL;

                memcpy( (char *)context->data + sizeof(*context->data), auth, auth_len );
                if (sig)
                    memcpy( (char *)context->data + sizeof(*context->data) + auth_len, sig, sig_size );
                free( auth ); free( sig ); free( pathA ); free( wurl );
            }

        _CLEANUP:

            if (FAILED(hr)) IXThreadingImpl_XAsyncComplete( xthreading, data->async, hr, 0 );
            else IXThreadingImpl_XAsyncComplete( xthreading, data->async, hr, data_size );
            hr = S_OK;
            break;

        case XAsyncOp_Cleanup:
            IUser_Release( user );
            free( context );
            break;

        case XAsyncOp_Cancel:
            break;
    }

    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureAsync( IXUserImpl6 *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, const char *method, const char *url, SIZE_T headerCount, const XUserGetTokenAndSignatureHttpHeader *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async )
{
    struct XUserGetTokenAndSignatureContext *context;
    IXThreadingImpl *xthreading;
    SIZE_T contextSize;
    HRESULT hr;
    char *ptr;

    TRACE( "iface %p, user %p, options %d, method %s, url %s, headerCount %Iu, headers %p, bodySize %Iu, bodyBuffer %p, async %p.\n", iface, user, options, debugstr_a( method ), debugstr_a( url ), headerCount, headers, bodySize, bodyBuffer, async );

    if (!method || !url || !async) return E_POINTER;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;

    contextSize = sizeof(*context) + headerCount * sizeof(*headers) + bodySize;
    contextSize += (strlen( method ) + strlen( url ) + 2) * sizeof(char);
    for (SIZE_T i = 0; i < headerCount; i++)
        contextSize += (strlen( headers[i].name ) + strlen( headers[i].value ) + 2) * sizeof(char);

    if (!(context = calloc( 1, contextSize )))
    {
        IXThreadingImpl_Release( xthreading );
        return E_OUTOFMEMORY;
    }

    if (FAILED(hr = IXUserImpl6_XUserDuplicateHandle( iface, user, &context->user )))
    {
        IXThreadingImpl_Release( xthreading );
        return hr;
    }

    context->isUtf16 = FALSE;
    context->options = options;
    context->bodySize = bodySize;
    context->headerCount = headerCount;

    context->headers = (XUserGetTokenAndSignatureHttpHeader *)context + sizeof(*context);
    ptr = (char *)context->headers + headerCount * sizeof(*headers);

    ptr += (strlen( strcpy( (context->method = ptr), method ) ) + 1) * sizeof(char);
    ptr += (strlen( strcpy( (context->url = ptr), url ) ) + 1) * sizeof(char);
    for (SIZE_T i = 0; i < headerCount; i++)
    {
        ptr += (strlen( strcpy( (char *)(context->headers[i].name = ptr), headers[i].name ) ) + 1) * sizeof(char);
        ptr += (strlen( strcpy( (char *)(context->headers[i].value = ptr), headers[i].value ) ) + 1) * sizeof(char);
    }
    memcpy( (context->bodyBuffer = ptr), bodyBuffer, bodySize );

    hr = IXThreadingImpl_XAsyncBegin( xthreading, async, context, NULL, "XUserGetTokenAndSignatureAsync", XUserGetTokenAndSignatureProvider );
    IXThreadingImpl_Release( xthreading );
    if (FAILED(hr)) free( context );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, async %p, bufferSize %p.\n", iface, async, bufferSize );

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResultSize( xthreading, async, bufferSize );
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureData **ptrToBuffer, SIZE_T *bufferUsed )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, async %p, bufferSize %Iu, buffer %p, ptrToBuffer %p, bufferUsed %p.\n", iface, async, bufferSize, buffer, ptrToBuffer, bufferUsed );

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult( xthreading, async, NULL, bufferSize, buffer, bufferUsed );
    if (SUCCEEDED(hr) && buffer)
    {
        /* The provider laid the buffer out as [struct][token][signature];
         * the token/signature pointers were NULL (their target only exists
         * now, in the caller's buffer). Point them at it — the game reads
         * data->token, so a NULL here made it retry forever. */
        XUserGetTokenAndSignatureData *d = buffer;
        d->token = (const char *)buffer + sizeof(*d);
        d->signature = d->signatureSize
            ? (const char *)buffer + sizeof(*d) + d->tokenSize : NULL;
    }
    *ptrToBuffer = (XUserGetTokenAndSignatureData *)buffer;
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Async( IXUserImpl6 *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, const WCHAR *method, const WCHAR *url, SIZE_T headerCount, const XUserGetTokenAndSignatureUtf16HttpHeader *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async )
{
    struct XUserGetTokenAndSignatureContext *context;
    IXThreadingImpl *xthreading;
    SIZE_T contextSize;
    HRESULT hr;
    WCHAR *ptr;

    TRACE( "iface %p, user %p, options %d, method %s, url %s, headerCount %Iu, headers %p, bodySize %Iu, bodyBuffer %p, async %p.\n", iface, user, options, debugstr_w( method ), debugstr_w( url ), headerCount, headers, bodySize, bodyBuffer, async );

    if (!method || !url || !async) return E_POINTER;
    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;

    contextSize = sizeof(*context) + headerCount * sizeof(*headers) + bodySize;
    contextSize += (wcslen( method ) + wcslen( url ) + 2) * sizeof(WCHAR);
    for (SIZE_T i = 0; i < headerCount; i++)
        contextSize += (wcslen( headers[i].name ) + wcslen( headers[i].value ) + 2) * sizeof(WCHAR);

    if (!(context = calloc( 1, contextSize )))
    {
        IXThreadingImpl_Release( xthreading );
        return E_OUTOFMEMORY;
    }

    if (FAILED(hr = IXUserImpl6_XUserDuplicateHandle( iface, user, &context->user )))
    {
        IXThreadingImpl_Release( xthreading );
        return hr;
    }

    context->isUtf16 = TRUE;
    context->options = options;
    context->bodySize = bodySize;
    context->headerCount = headerCount;

    context->headersUtf16 = (XUserGetTokenAndSignatureUtf16HttpHeader *)context + sizeof(*context);
    ptr = (WCHAR *)context->headersUtf16 + headerCount * sizeof(*headers);

    ptr += (wcslen( wcscpy( (context->methodUtf16 = ptr), method ) ) + 1) * sizeof(WCHAR);
    ptr += (wcslen( wcscpy( (context->urlUtf16 = ptr), url ) ) + 1) * sizeof(WCHAR);
    for (SIZE_T i = 0; i < headerCount; i++)
    {
        ptr += (wcslen( wcscpy( (WCHAR *)(context->headersUtf16[i].name = ptr), headers[i].name ) ) + 1) * sizeof(WCHAR);
        ptr += (wcslen( wcscpy( (WCHAR *)(context->headersUtf16[i].value = ptr), headers[i].value ) ) + 1) * sizeof(WCHAR);
    }
    memcpy( (context->bodyBuffer = ptr), bodyBuffer, bodySize );

    hr = IXThreadingImpl_XAsyncBegin( xthreading, async, context, NULL, "XUserGetTokenAndSignatureUtf16Async", XUserGetTokenAndSignatureProvider );
    IXThreadingImpl_Release( xthreading );
    if (FAILED(hr)) free( context );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16ResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, async %p, bufferSize %p.\n", iface, async, bufferSize );

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResultSize( xthreading, async, bufferSize );
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Result( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureUtf16Data **ptrToBuffer, SIZE_T *bufferUsed )
{
    IXThreadingImpl *xthreading;
    HRESULT hr;

    TRACE( "iface %p, async %p, bufferSize %Iu, buffer %p, ptrToBuffer %p, bufferUsed %p.\n", iface, async, bufferSize, buffer, ptrToBuffer, bufferUsed );

    if (FAILED(hr = QueryApiImpl( &CLSID_XThreadingImpl, &IID_IXThreadingImpl, (void **)&xthreading ))) return hr;
    hr = IXThreadingImpl_XAsyncGetResult( xthreading, async, NULL, bufferSize, buffer, bufferUsed );
    if (SUCCEEDED(hr) && buffer)
    {
        XUserGetTokenAndSignatureUtf16Data *d = buffer;
        d->token = (const WCHAR *)((char *)buffer + sizeof(*d));
        d->signature = d->signatureCount
            ? (const WCHAR *)((char *)buffer + sizeof(*d)
                              + d->tokenCount * sizeof(WCHAR)) : NULL;
    }
    *ptrToBuffer = (XUserGetTokenAndSignatureUtf16Data *)buffer;
    IXThreadingImpl_Release( xthreading );
    return hr;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiAsync( IXUserImpl6 *iface, XUserHandle user, const char *url, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, url %s, async %p stub!\n", iface, user, debugstr_a( url ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Async( IXUserImpl6 *iface, XUserHandle user, const WCHAR *url, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, url %s, async %p stub!\n", iface, user, debugstr_w( url ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Result( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserRegisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueHandle queue, void *context, XUserChangeEventCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserUnregisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_user_XUserGetSignOutDeferral( IXUserImpl6 *iface, XUserSignOutDeferralHandle *deferral )
{
    TRACE( "iface %p, deferral %p.\n", iface, deferral );
    *deferral = NULL;
    return E_GAMEUSER_DEFERRAL_NOT_AVAILABLE;
}

static void WINAPI x_user_XUserCloseSignOutDeferralHandle( IXUserImpl6 *iface, XUserSignOutDeferralHandle deferral )
{
    TRACE( "iface %p, deferral %p.\n", iface, deferral );
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiAsync( IXUserImpl6 *iface, UINT64 userId, XAsyncBlock *async )
{
    FIXME( "iface %p, userId %llu, async %p stub!\n", iface, userId, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async, XUserHandle *newUser )
{
    FIXME( "iface %p, async %p, newUser %p stub!\n", iface, async, newUser );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyAsync( IXUserImpl6 *iface, XUserHandle user, XUserGetMsaTokenSilentlyOptions options, const char *scope, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, options %u, scope %s, async %p stub!\n", iface, user, options, debugstr_a( scope ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T resultTokenSize, char *resultToken, SIZE_T *resultTokenUsed )
{
    FIXME( "iface %p, async %p, resultTokenSize %Iu, resultToken %p, resultTokenUsed %p stub!\n", iface, async, resultTokenSize, resultToken, resultTokenUsed );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *tokenSize )
{
    FIXME( "iface %p, async %p, tokenSize %p stub!\n", iface, async, tokenSize );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsStoreUser( IXUserImpl6 *iface, XUserHandle user )
{
    FIXME( "iface %p, user %p stub!\n", iface, user );
    return TRUE;
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectSetEventHandlers( IXUserImpl6 *iface, XTaskQueueHandle queue, XUserPlatformRemoteConnectEventHandlers *handlers )
{
    FIXME( "iface %p, queue %p, handlers %p stub!\n", iface, queue, handlers );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectCancelPrompt( IXUserImpl6 *iface, XUserPlatformOperation operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptSetEventHandlers( IXUserImpl6 *iface, XTaskQueueHandle queue, XUserPlatformSpopPromptEventHandler *handler, void *context )
{
    FIXME( "iface %p, queue %p, handler %p, context %p stub!\n", iface, queue, handler, context );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptComplete( IXUserImpl6 *iface, XUserPlatformOperation operation, XUserPlatformOperationResult result )
{
    FIXME( "iface %p, operation %p, result %d stub!\n", iface, operation, result );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsSignOutPresent( IXUserImpl6 *iface )
{
    TRACE( "iface %p.\n", iface );
    return FALSE;
}

static HRESULT WINAPI x_user_XUserSignOutAsync( IXUserImpl6 *iface, XUserHandle user, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, async %p stub!\n", iface, user, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserSignOutResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static const struct IXUserImpl6Vtbl x_user_vtbl =
{
    x_user_QueryInterface,
    x_user_AddRef,
    x_user_Release,
    /* IXUserImpl methods */
    x_user_XUserDuplicateHandle,
    x_user_XUserCloseHandle,
    x_user_XUserCompare,
    x_user_XUserGetMaxUsers,
    x_user_XUserAddAsync,
    x_user_XUserAddResult,
    x_user_XUserGetLocalId,
    x_user_XUserFindUserByLocalId,
    x_user_XUserGetId,
    x_user_XUserFindUserById,
    x_user_XUserGetIsGuest,
    x_user_XUserGetState,
    __PADDING__,
    x_user_XUserGetGamerPictureAsync,
    x_user_XUserGetGamerPictureResultSize,
    x_user_XUserGetGamerPictureResult,
    x_user_XUserGetAgeGroup,
    x_user_XUserCheckPrivilege,
    x_user_XUserResolvePrivilegeWithUiAsync,
    x_user_XUserResolvePrivilegeWithUiResult,
    x_user_XUserGetTokenAndSignatureAsync,
    x_user_XUserGetTokenAndSignatureResultSize,
    x_user_XUserGetTokenAndSignatureResult,
    x_user_XUserGetTokenAndSignatureUtf16Async,
    x_user_XUserGetTokenAndSignatureUtf16ResultSize,
    x_user_XUserGetTokenAndSignatureUtf16Result,
    x_user_XUserResolveIssueWithUiAsync,
    x_user_XUserResolveIssueWithUiResult,
    x_user_XUserResolveIssueWithUiUtf16Async,
    x_user_XUserResolveIssueWithUiUtf16Result,
    x_user_XUserRegisterForChangeEvent,
    x_user_XUserUnregisterForChangeEvent,
    x_user_XUserGetSignOutDeferral,
    x_user_XUserCloseSignOutDeferralHandle,
    /* IXUserImpl2 methods */
    x_user_XUserAddByIdWithUiAsync,
    x_user_XUserAddByIdWithUiResult,
    /* IXUserImpl3 methods */
    x_user_XUserGetMsaTokenSilentlyAsync,
    x_user_XUserGetMsaTokenSilentlyResult,
    x_user_XUserGetMsaTokenSilentlyResultSize,
    /* IXUserImpl4 methods */
    x_user_XUserIsStoreUser,
    /* IXUserImpl5 methods */
    x_user_XUserPlatformRemoteConnectSetEventHandlers,
    x_user_XUserPlatformRemoteConnectCancelPrompt,
    x_user_XUserPlatformSpopPromptSetEventHandlers,
    x_user_XUserPlatformSpopPromptComplete,
    /* IXUserImpl6 methods */
    x_user_XUserIsSignOutPresent,
    x_user_XUserSignOutAsync,
    x_user_XUserSignOutResult,
};

static inline struct x_user *impl_from_IXUserGamertagImpl( IXUserGamertagImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserGamertagImpl_iface );
}

static HRESULT WINAPI x_user_gamertag_QueryInterface( IXUserGamertagImpl *iface, REFIID riid, void **out )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_QueryInterface( &impl->IXUserImpl_iface, riid, out );
}

static ULONG WINAPI x_user_gamertag_AddRef( IXUserGamertagImpl *iface )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_AddRef( &impl->IXUserImpl_iface );
}

static ULONG WINAPI x_user_gamertag_Release( IXUserGamertagImpl *iface )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_Release( &impl->IXUserImpl_iface );
}

static HRESULT WINAPI x_user_gamertag_XUserGetGamertag( IXUserGamertagImpl *iface, XUserHandle user, XUserGamertagComponent gamertagComponent, SIZE_T gamertagSize, char *gamertag, SIZE_T *gamertagUsed )
{
    FIXME( "iface %p, user %p, gamertagComponent %d, gamertagSize %Iu, gamertag %p, gamertagUsed %p semi-stub!\n", iface, user, gamertagComponent, gamertagSize, gamertag, gamertagUsed );

    switch (gamertagComponent)
    {
        case XUserGamertagComponent_Classic:
            if (gamertagSize <= user->classic_gamertag_size)
                return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );

            memcpy( gamertag, user->classic_gamertag, user->classic_gamertag_size );
            gamertag[user->classic_gamertag_size] = 0;
            if (gamertagUsed) *gamertagUsed = user->classic_gamertag_size + 1;
            return S_OK;

        default:
            /* TODO: handle modern, modern suffix & unique modern components */
            break;
    }

    return E_INVALIDARG;
}

static const struct IXUserGamertagImplVtbl x_user_gamertag_vtbl =
{
    x_user_gamertag_QueryInterface,
    x_user_gamertag_AddRef,
    x_user_gamertag_Release,
    /* IXUserGamertagImpl methods */
    x_user_gamertag_XUserGetGamertag,
};

struct x_user_device
{
    IXUserDeviceImpl IXUserDeviceImpl_iface;
    LONG ref;
};

static inline struct x_user_device *impl_from_IXUserDeviceImpl( IXUserDeviceImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_user_device, IXUserDeviceImpl_iface );
}

static HRESULT WINAPI x_user_device_QueryInterface( IXUserDeviceImpl *iface, REFIID iid, void **out )
{
    struct x_user_device *impl = impl_from_IXUserDeviceImpl( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown         ) ||
        IsEqualGUID( iid, &IID_IXUserDeviceImpl ))
    {
        IXUserDeviceImpl_AddRef( *out = &impl->IXUserDeviceImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_device_AddRef( IXUserDeviceImpl *iface )
{
    struct x_user_device *impl = impl_from_IXUserDeviceImpl( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_user_device_Release( IXUserDeviceImpl *iface )
{
    struct x_user_device *impl = impl_from_IXUserDeviceImpl( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_user_device_XUserFindForDevice( IXUserDeviceImpl *iface, const APP_LOCAL_DEVICE_ID *deviceId, XUserHandle *handle )
{
    FIXME( "iface %p, deviceId %p, handle %p stub!\n", iface, deviceId, handle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserRegisterForDeviceAssociationChanged( IXUserDeviceImpl *iface, XTaskQueueHandle queue, void *context, XUserDeviceAssociationChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_device_XUserUnregisterForDeviceAssociationChanged( IXUserDeviceImpl *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_user_device_XUserGetDefaultAudioEndpointUtf16( IXUserDeviceImpl *iface, XUserLocalId user, XUserDefaultAudioEndpointKind defaultAudioEndpointKind, SIZE_T endpointIdUtf16Count, WCHAR *endpointIdUtf16, SIZE_T *endpointIdUtf16Used )
{
    FIXME( "iface %p, user %p, defaultAudioEndpointKind %d, endpointIdUtf16Count %Iu, endpointIdUtf16 %p, endpointIdUtf16used %p stub!\n", iface, &user, defaultAudioEndpointKind, endpointIdUtf16Count, endpointIdUtf16, endpointIdUtf16Used );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserRegisterForDefaultAudioEndpointUtf16Changed( IXUserDeviceImpl *iface, XTaskQueueHandle queue, void *context, XUserDefaultAudioEndpointUtf16ChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_device_XUserUnregisterForDefaultAudioEndpointUtf16Changed( IXUserDeviceImpl *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_user_device_XUserFindControllerForUserWithUiAsync( IXUserDeviceImpl *iface, XUserHandle user, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, async %p stub!\n", iface, user, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserFindControllerForUserWithUiResult( IXUserDeviceImpl *iface, XAsyncBlock *async, APP_LOCAL_DEVICE_ID *deviceId )
{
    FIXME( "iface %p, async %p, deviceId %p stub!\n", iface, async, deviceId );
    return E_NOTIMPL;
}

static const struct IXUserDeviceImplVtbl x_user_device_vtbl =
{
    x_user_device_QueryInterface,
    x_user_device_AddRef,
    x_user_device_Release,
    /* IXUserDeviceImpl methods */
    x_user_device_XUserFindForDevice,
    x_user_device_XUserRegisterForDeviceAssociationChanged,
    x_user_device_XUserUnregisterForDeviceAssociationChanged,
    x_user_device_XUserGetDefaultAudioEndpointUtf16,
    x_user_device_XUserRegisterForDefaultAudioEndpointUtf16Changed,
    x_user_device_XUserUnregisterForDefaultAudioEndpointUtf16Changed,
    x_user_device_XUserFindControllerForUserWithUiAsync,
    x_user_device_XUserFindControllerForUserWithUiResult,
};

static struct x_user x_user =
{
    {&x_user_vtbl},
    {&x_user_gamertag_vtbl},
    0,
};

static struct x_user_device x_user_device =
{
    {&x_user_device_vtbl},
    0,
};

IXUserImpl6 *x_user_impl = &x_user.IXUserImpl_iface;
IXUserDeviceImpl *x_user_device_impl = &x_user_device.IXUserDeviceImpl_iface;
