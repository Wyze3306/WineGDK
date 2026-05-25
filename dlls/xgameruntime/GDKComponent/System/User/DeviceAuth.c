/*
 * Xbox Device Authentication and Request Signing
 *
 * Generates EC P-256 key pair, requests device token from Xbox Live,
 * and computes ECDSA request signatures for XBL3.0 authenticated requests.
 *
 * Reference: MinecraftAuth library (dev.kastle), gophertunnel (Sandertv)
 */

#include "DeviceAuth.h"
#include "Token.h"

/* Re-declare the GetJsonStringValue helper locally */
static inline HRESULT GetJsonStringValueLocal( IJsonObject *object, LPCWSTR key, HSTRING *value )
{
    HSTRING_HEADER key_hdr;
    HSTRING key_hstr;
    HRESULT hr;
    if (FAILED( hr = WindowsCreateStringReference( key, wcslen( key ), &key_hdr, &key_hstr ) ))
        return hr;
    return IJsonObject_GetNamedString( object, key_hstr, value );
}
#define GetJsonStringValue GetJsonStringValueLocal

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static BCRYPT_KEY_HANDLE g_device_key;
static BCRYPT_ALG_HANDLE g_ecdsa_alg;
static BCRYPT_ALG_HANDLE g_sha256_alg;
static BYTE g_pubkey_x[32];
static BYTE g_pubkey_y[32];
static HSTRING g_device_token;
static BOOLEAN g_initialized;
static CHAR g_device_id[40]; /* {UUID} */

/* Base64url encode without padding */
static void base64url_encode( const BYTE *data, SIZE_T len, LPSTR out, SIZE_T out_size )
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    SIZE_T i, j = 0;
    for (i = 0; i + 2 < len; i += 3)
    {
        if (j + 4 >= out_size) break;
        out[j++] = table[(data[i] >> 2) & 0x3F];
        out[j++] = table[((data[i] & 0x3) << 4) | ((data[i+1] >> 4) & 0xF)];
        out[j++] = table[((data[i+1] & 0xF) << 2) | ((data[i+2] >> 6) & 0x3)];
        out[j++] = table[data[i+2] & 0x3F];
    }
    if (i < len)
    {
        out[j++] = table[(data[i] >> 2) & 0x3F];
        if (i + 1 < len)
        {
            out[j++] = table[((data[i] & 0x3) << 4) | ((data[i+1] >> 4) & 0xF)];
            out[j++] = table[((data[i+1] & 0xF) << 2)];
        }
        else
        {
            out[j++] = table[((data[i] & 0x3) << 4)];
        }
    }
    out[j] = '\0';

    /* Convert + to - and / to _ for URL-safe */
    for (i = 0; i < j; i++)
    {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }
}

/* Standard base64 encode (with padding) */
static void base64_encode( const BYTE *data, SIZE_T len, LPSTR out, SIZE_T out_size )
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    SIZE_T i, j = 0;
    for (i = 0; i + 2 < len; i += 3)
    {
        if (j + 4 >= out_size) break;
        out[j++] = table[(data[i] >> 2) & 0x3F];
        out[j++] = table[((data[i] & 0x3) << 4) | ((data[i+1] >> 4) & 0xF)];
        out[j++] = table[((data[i+1] & 0xF) << 2) | ((data[i+2] >> 6) & 0x3)];
        out[j++] = table[data[i+2] & 0x3F];
    }
    if (i < len)
    {
        out[j++] = table[(data[i] >> 2) & 0x3F];
        if (i + 1 < len)
        {
            out[j++] = table[((data[i] & 0x3) << 4) | ((data[i+1] >> 4) & 0xF)];
            out[j++] = table[((data[i+1] & 0xF) << 2)];
            out[j++] = '=';
        }
        else
        {
            out[j++] = table[((data[i] & 0x3) << 4)];
            out[j++] = '=';
            out[j++] = '=';
        }
    }
    out[j] = '\0';
}

/* Write big-endian int32 */
static void write_be32( BYTE *buf, UINT32 val )
{
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

/* Write big-endian int64 */
static void write_be64( BYTE *buf, INT64 val )
{
    buf[0] = (val >> 56) & 0xFF;
    buf[1] = (val >> 48) & 0xFF;
    buf[2] = (val >> 40) & 0xFF;
    buf[3] = (val >> 32) & 0xFF;
    buf[4] = (val >> 24) & 0xFF;
    buf[5] = (val >> 16) & 0xFF;
    buf[6] = (val >> 8) & 0xFF;
    buf[7] = val & 0xFF;
}

/* Generate UUID string {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} */
static void generate_uuid( LPSTR buf )
{
    GUID guid;
    CoCreateGuid( &guid );
    sprintf( buf, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
             guid.Data1, guid.Data2, guid.Data3,
             guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
             guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7] );
}

HRESULT DeviceAuth_GetProofKeyJson( LPSTR *json )
{
    CHAR x_b64[64], y_b64[64];
    SIZE_T len;

    base64url_encode( g_pubkey_x, 32, x_b64, sizeof(x_b64) );
    base64url_encode( g_pubkey_y, 32, y_b64, sizeof(y_b64) );

    len = 128 + strlen(x_b64) + strlen(y_b64);
    *json = calloc( 1, len );
    if (!*json) return E_OUTOFMEMORY;

    sprintf( *json, "{\"kty\":\"EC\",\"alg\":\"ES256\",\"crv\":\"P-256\",\"use\":\"sig\",\"x\":\"%s\",\"y\":\"%s\"}", x_b64, y_b64 );
    return S_OK;
}

BOOLEAN DeviceAuth_IsInitialized( void )
{
    return g_initialized;
}

HRESULT DeviceAuth_GetDeviceToken( HSTRING *token )
{
    if (!g_initialized || !g_device_token) return E_FAIL;
    return WindowsDuplicateString( g_device_token, token );
}

HRESULT DeviceAuth_SignRequest( LPCSTR method, LPCSTR url_path, LPCSTR auth_header,
                                 LPCSTR body, SIZE_T body_len,
                                 LPSTR *signature_b64 )
{
    BYTE hash_input[16384];
    BYTE hash[32];
    BYTE sig[64];
    BYTE header_blob[76];
    ULONG sig_len;
    SIZE_T pos = 0;
    FILETIME ft;
    ULARGE_INTEGER timestamp;
    NTSTATUS status;

    if (!g_device_key) return E_FAIL;

    GetSystemTimeAsFileTime( &ft );
    timestamp.LowPart = ft.dwLowDateTime;
    timestamp.HighPart = ft.dwHighDateTime;

    /* Build hash input */
    write_be32( hash_input + pos, 1 ); pos += 4; /* policy version */
    hash_input[pos++] = 0;
    write_be64( hash_input + pos, timestamp.QuadPart ); pos += 8;
    hash_input[pos++] = 0;
    if (method) { memcpy( hash_input + pos, method, strlen(method) ); pos += strlen(method); }
    hash_input[pos++] = 0;
    if (url_path) { memcpy( hash_input + pos, url_path, strlen(url_path) ); pos += strlen(url_path); }
    hash_input[pos++] = 0;
    if (auth_header) { memcpy( hash_input + pos, auth_header, strlen(auth_header) ); pos += strlen(auth_header); }
    hash_input[pos++] = 0;
    if (body && body_len > 0 && pos + body_len < sizeof(hash_input) - 1)
    {
        memcpy( hash_input + pos, body, body_len );
        pos += body_len;
    }
    hash_input[pos++] = 0;

    /* SHA-256 hash */
    status = BCryptHash( g_sha256_alg, NULL, 0, hash_input, pos, hash, 32 );
    if (status)
    {
        WARN( "BCryptHash failed: 0x%08lx\n", status );
        return HRESULT_FROM_NT( status );
    }

    /* ECDSA sign - P1363 format (r[32] || s[32]) */
    status = BCryptSignHash( g_device_key, NULL, hash, 32, sig, 64, &sig_len, 0 );
    if (status)
    {
        WARN( "BCryptSignHash failed: 0x%08lx\n", status );
        return HRESULT_FROM_NT( status );
    }

    /* Build signature header: version(4) + timestamp(8) + sig(64) = 76 bytes */
    write_be32( header_blob, 1 );
    write_be64( header_blob + 4, timestamp.QuadPart );
    memcpy( header_blob + 12, sig, 64 );

    *signature_b64 = calloc( 1, 128 );
    if (!*signature_b64) return E_OUTOFMEMORY;
    base64_encode( header_blob, 76, *signature_b64, 128 );

    TRACE( "signed request: method=%s path=%s sig=%.20s...\n", method, url_path, *signature_b64 );
    return S_OK;
}

HRESULT DeviceAuth_Initialize( LPCSTR msa_access_token )
{
    NTSTATUS status;
    BYTE blob[256];
    ULONG blob_size;
    BCRYPT_ECCKEY_BLOB *ecc_blob;
    LPCWSTR accept[] = {L"application/json", NULL};
    LPSTR proof_key_json = NULL;
    LPSTR device_auth_body = NULL;
    LPSTR sig_header = NULL;
    LPSTR response = NULL;
    SIZE_T response_size;
    HRESULT hr;

    if (g_initialized) return S_OK;

    TRACE( "initializing device auth\n" );

    /* Open algorithms */
    status = BCryptOpenAlgorithmProvider( &g_ecdsa_alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0 );
    if (status) { WARN( "BCryptOpenAlgorithmProvider ECDSA failed: 0x%08lx\n", status ); return HRESULT_FROM_NT(status); }

    status = BCryptOpenAlgorithmProvider( &g_sha256_alg, BCRYPT_SHA256_ALGORITHM, NULL, 0 );
    if (status) { WARN( "BCryptOpenAlgorithmProvider SHA256 failed: 0x%08lx\n", status ); return HRESULT_FROM_NT(status); }

    /* Pre-auth bypass: Wine 11.1's GnuTLS schannel ClientHello is
     * fingerprinted by Microsoft Azure — *.auth.xboxlive.com TCP-RSTs the
     * HTTP body right after the handshake, breaking our /device/authenticate
     * POST with 0x80072746 and surfacing as MC's "Llama 0x80072746" dialog.
     * The host-side launcher's xbl_preauth() does the same call with the
     * system OpenSSL stack and persists {device_id, ecc_private_blob, device_
     * token} as a JSON file whose Wine path is in WINEGDK_PREAUTH_DEVICE.
     * When that file exists we just import the EC key + device token from
     * it and skip the broken POST. */
    {
        const char *preauth_path = getenv( "WINEGDK_PREAUTH_DEVICE" );
        if (preauth_path && *preauth_path)
        {
            HANDLE fh = CreateFileA( preauth_path, GENERIC_READ, FILE_SHARE_READ,
                                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
            if (fh != INVALID_HANDLE_VALUE)
            {
                DWORD sz = GetFileSize( fh, NULL ), rd = 0;
                if (sz > 0 && sz < 64 * 1024)
                {
                    LPSTR buf = calloc( 1, sz + 1 );
                    if (buf && ReadFile( fh, buf, sz, &rd, NULL ) && rd == sz)
                    {
                        IJsonObject *root = NULL;
                        HSTRING tok = NULL, blob_b64 = NULL, devid = NULL;
                        if (SUCCEEDED( ParseJsonObject( buf, sz, &root ) ))
                        {
                            (void)GetJsonStringValue( root, L"device_token", &tok );
                            (void)GetJsonStringValue( root, L"ecc_private_blob_b64", &blob_b64 );
                            (void)GetJsonStringValue( root, L"device_id", &devid );
                        }
                        if (tok && blob_b64)
                        {
                            /* base64-decode the ECC private blob and import */
                            LPSTR blob_mb = NULL; UINT32 blob_len = 0;
                            HSTRINGToMultiByte( blob_b64, &blob_mb, &blob_len );
                            DWORD raw_size = 0;
                            CryptStringToBinaryA( blob_mb, blob_len, CRYPT_STRING_BASE64,
                                                  NULL, &raw_size, NULL, NULL );
                            BYTE *raw = malloc( raw_size );
                            if (raw && CryptStringToBinaryA( blob_mb, blob_len, CRYPT_STRING_BASE64,
                                                              raw, &raw_size, NULL, NULL )
                                && BCryptImportKeyPair( g_ecdsa_alg, NULL,
                                                         BCRYPT_ECCPRIVATE_BLOB, &g_device_key,
                                                         raw, raw_size, 0 ) == 0)
                            {
                                /* Cache public key components for proof-key building */
                                if (BCryptExportKey( g_device_key, NULL, BCRYPT_ECCPUBLIC_BLOB,
                                                      blob, sizeof(blob), &blob_size, 0 ) == 0)
                                {
                                    memcpy( g_pubkey_x, blob + sizeof(BCRYPT_ECCKEY_BLOB), 32 );
                                    memcpy( g_pubkey_y, blob + sizeof(BCRYPT_ECCKEY_BLOB) + 32, 32 );
                                }
                                if (devid)
                                {
                                    LPSTR id_mb = NULL; UINT32 id_len = 0;
                                    HSTRINGToMultiByte( devid, &id_mb, &id_len );
                                    if (id_mb && id_len < sizeof(g_device_id))
                                        memcpy( g_device_id, id_mb, id_len + 1 );
                                    free( id_mb );
                                }
                                /* Steal the device token straight from the JSON */
                                WindowsDuplicateString( tok, &g_device_token );
                                g_initialized = TRUE;
                                ERR( "preauth: loaded device token (%u-byte ECC blob) for id=%s — skipping Wine HTTP\n",
                                     raw_size, g_device_id );
                            }
                            free( raw );
                            free( blob_mb );
                        }
                        if (tok) WindowsDeleteString( tok );
                        if (blob_b64) WindowsDeleteString( blob_b64 );
                        if (devid) WindowsDeleteString( devid );
                        if (root) IJsonObject_Release( root );
                    }
                    free( buf );
                }
                CloseHandle( fh );
                if (g_initialized) return S_OK;
            }
            else WARN( "preauth: WINEGDK_PREAUTH_DEVICE=%s but file unreadable\n", preauth_path );
        }
    }

    /* Try to reuse the device identity persisted from a previous run.
     * Microsoft tracks "trusted devices" per Xbox account; regenerating a
     * fresh UUID + EC key pair on every launch tells SISU we're a brand
     * new device every time, and the AuthorizationToken stays provisional
     * (WebPage in the response).  Persisting the UUID + ECCPRIVATE blob
     * in HKCU\\Software\\Wine\\WineGDK\\Device lets MS see the same
     * device repeated and trust may eventually establish. */
    {
        HKEY hKey;
        DWORD id_size = sizeof( g_device_id );
        BYTE key_blob[256];
        DWORD key_size = sizeof( key_blob );
        DWORD type;
        BOOLEAN loaded = FALSE;
        if (RegOpenKeyExA( HKEY_CURRENT_USER,
                           "Software\\Wine\\WineGDK\\Device", 0, KEY_READ,
                           &hKey ) == ERROR_SUCCESS)
        {
            if (RegQueryValueExA( hKey, "DeviceId", NULL, &type,
                                   (LPBYTE)g_device_id, &id_size ) == ERROR_SUCCESS
                && type == REG_SZ
                && RegQueryValueExA( hKey, "PrivateKey", NULL, &type,
                                      key_blob, &key_size ) == ERROR_SUCCESS
                && type == REG_BINARY
                && BCryptImportKeyPair( g_ecdsa_alg, NULL,
                                         BCRYPT_ECCPRIVATE_BLOB, &g_device_key,
                                         key_blob, key_size, 0 ) == 0)
            {
                /* Export public key for ProofKey */
                if (BCryptExportKey( g_device_key, NULL, BCRYPT_ECCPUBLIC_BLOB,
                                      blob, sizeof(blob), &blob_size, 0 ) == 0)
                {
                    memcpy( g_pubkey_x, blob + sizeof(BCRYPT_ECCKEY_BLOB), 32 );
                    memcpy( g_pubkey_y, blob + sizeof(BCRYPT_ECCKEY_BLOB) + 32, 32 );
                    loaded = TRUE;
                    TRACE( "reusing persisted device id=%s\n", g_device_id );
                }
                else
                {
                    BCryptDestroyKey( g_device_key );
                    g_device_key = NULL;
                }
            }
            RegCloseKey( hKey );
        }

        if (!loaded)
        {
            /* Generate P-256 key pair */
            status = BCryptGenerateKeyPair( g_ecdsa_alg, &g_device_key, 256, 0 );
            if (status) { WARN( "BCryptGenerateKeyPair failed: 0x%08lx\n", status ); return HRESULT_FROM_NT(status); }

            status = BCryptFinalizeKeyPair( g_device_key, 0 );
            if (status) { WARN( "BCryptFinalizeKeyPair failed: 0x%08lx\n", status ); return HRESULT_FROM_NT(status); }

            /* Export public key */
            status = BCryptExportKey( g_device_key, NULL, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &blob_size, 0 );
            if (status) { WARN( "BCryptExportKey failed: 0x%08lx\n", status ); return HRESULT_FROM_NT(status); }

            ecc_blob = (BCRYPT_ECCKEY_BLOB *)blob;
            memcpy( g_pubkey_x, blob + sizeof(BCRYPT_ECCKEY_BLOB), 32 );
            memcpy( g_pubkey_y, blob + sizeof(BCRYPT_ECCKEY_BLOB) + 32, 32 );

            TRACE( "generated EC P-256 key pair, cbKey=%lu\n", ecc_blob->cbKey );

            /* Generate device ID */
            generate_uuid( g_device_id );

            /* Persist for the next launch.  Export ECCPRIVATE_BLOB (108 bytes
             * for P-256: 8-byte header + X[32] + Y[32] + D[32]) so we can
             * BCryptImportKeyPair it next time.  Best-effort; failures here
             * just mean next launch regenerates. */
            {
                BYTE priv_blob[256];
                ULONG priv_size = 0;
                if (BCryptExportKey( g_device_key, NULL, BCRYPT_ECCPRIVATE_BLOB,
                                      priv_blob, sizeof(priv_blob), &priv_size, 0 ) == 0
                    && RegCreateKeyExA( HKEY_CURRENT_USER,
                                         "Software\\Wine\\WineGDK\\Device", 0, NULL,
                                         REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL,
                                         &hKey, NULL ) == ERROR_SUCCESS)
                {
                    RegSetValueExA( hKey, "DeviceId", 0, REG_SZ,
                                     (const BYTE *)g_device_id,
                                     strlen( g_device_id ) + 1 );
                    RegSetValueExA( hKey, "PrivateKey", 0, REG_BINARY,
                                     priv_blob, priv_size );
                    RegCloseKey( hKey );
                    TRACE( "persisted device id=%s + EC private key (%lu bytes)\n",
                           g_device_id, priv_size );
                }
            }
        }
    }

    /* Get ProofKey JSON */
    hr = DeviceAuth_GetProofKeyJson( &proof_key_json );
    if (FAILED(hr)) return hr;

    /* Build device authenticate request body */
    {
        SIZE_T body_len = 512 + strlen(proof_key_json);
        device_auth_body = calloc( 1, body_len );
        if (!device_auth_body) { free(proof_key_json); return E_OUTOFMEMORY; }
        sprintf( device_auth_body,
            "{\"Properties\":{\"AuthMethod\":\"ProofOfPossession\","
            "\"Id\":\"%s\","
            "\"DeviceType\":\"Win32\","
            "\"Version\":\"10.0.25398.4909\","
            "\"ProofKey\":%s},"
            "\"RelyingParty\":\"http://auth.xboxlive.com\","
            "\"TokenType\":\"JWT\"}",
            g_device_id, proof_key_json );
        free( proof_key_json );
    }

    /* Sign the device auth request */
    hr = DeviceAuth_SignRequest( "POST", "/device/authenticate", "",
                                  device_auth_body, strlen(device_auth_body), &sig_header );
    if (FAILED(hr))
    {
        WARN( "failed to sign device auth request: 0x%08lx\n", hr );
        free( device_auth_body );
        return hr;
    }

    /* Build extra headers: Signature + x-xbl-contract-version */
    {
        WCHAR sig_hdr_w[256];
        WCHAR headers[512];
        MultiByteToWideChar( CP_UTF8, 0, sig_header, -1, sig_hdr_w, 256 );
        swprintf( headers, 512, L"content-type: application/json\r\nSignature: %s\r\nx-xbl-contract-version: 1", sig_hdr_w );
        free( sig_header );

        hr = HttpRequest(
            L"POST",
            L"device.auth.xboxlive.com",
            L"/device/authenticate",
            device_auth_body,
            headers,
            accept,
            &response,
            &response_size
        );
    }

    free( device_auth_body );

    if (FAILED(hr))
    {
        WARN( "device auth HTTP request failed: 0x%08lx\n", hr );
        return hr;
    }

    TRACE( "device auth response (%llu bytes): %.200s\n", (unsigned long long)response_size, response );

    /* Parse response to get device token */
    {
        IJsonObject *object;
        hr = ParseJsonObject( response, response_size, &object );
        free( response );
        if (FAILED(hr))
        {
            WARN( "failed to parse device auth response: 0x%08lx\n", hr );
            return hr;
        }

        hr = GetJsonStringValue( object, L"Token", &g_device_token );
        IJsonObject_Release( object );
        if (FAILED(hr))
        {
            WARN( "failed to get Token from device auth response: 0x%08lx\n", hr );
            return hr;
        }
    }

    g_initialized = TRUE;
    TRACE( "device auth initialized successfully\n" );
    return S_OK;
}
