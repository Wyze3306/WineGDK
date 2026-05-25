/*
 * Xbox Device Authentication and Request Signing
 * Implements EC P-256 key pair, device token, and XBL request signatures
 */

#ifndef DEVICEAUTH_H
#define DEVICEAUTH_H

#include "../../../private.h"
#include <bcrypt.h>
#include <wincrypt.h>

/* Initialize device auth - generate key pair and get device token */
HRESULT DeviceAuth_Initialize( LPCSTR msa_access_token );

/* Get the device token (JWT string) */
HRESULT DeviceAuth_GetDeviceToken( HSTRING *token );

/* Compute XBL request signature for an HTTP request */
HRESULT DeviceAuth_SignRequest( LPCSTR method, LPCSTR url_path, LPCSTR auth_header,
                                 LPCSTR body, SIZE_T body_len,
                                 LPSTR *signature_b64 );

/* Get the ProofKey JWK as a JSON string */
HRESULT DeviceAuth_GetProofKeyJson( LPSTR *json );

/* Check if device auth is initialized */
BOOLEAN DeviceAuth_IsInitialized( void );

#endif
