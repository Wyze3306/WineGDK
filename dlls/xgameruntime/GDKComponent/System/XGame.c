/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XGame
 *
 * Copyright 2026 Olivia Ryan
 * Copyright 2026 the WineGDK contributors
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

#include "private.h"

#include <string.h>
#include <wchar.h>

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

#define GAME_CONFIG_MAX_SIZE (4 * 1024 * 1024)

UINT32 winegdk_game_title_id;
char winegdk_game_msa_app_id[17];
BOOLEAN winegdk_game_msa_full_trust;

static INIT_ONCE game_config_once = INIT_ONCE_STATIC_INIT;
static HRESULT game_config_status = E_GAME_MISSING_GAME_CONFIG;

struct x_game
{
    IXGameImpl3 IXGameImpl3_iface;
    LONG ref;
};

static inline struct x_game *impl_from_IXGameImpl3( IXGameImpl3 *iface )
{
    return CONTAINING_RECORD( iface, struct x_game, IXGameImpl3_iface );
}

static BOOL ascii_space( char value )
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static WCHAR *last_path_separator( WCHAR *path )
{
    WCHAR *backslash = wcsrchr( path, '\\' );
    WCHAR *slash = wcsrchr( path, '/' );

    if (!backslash) return slash;
    if (!slash) return backslash;
    return slash > backslash ? slash : backslash;
}

static BOOL build_config_path( const WCHAR *directory, const WCHAR *name,
                               WCHAR *path, SIZE_T capacity )
{
    SIZE_T directory_length = wcslen( directory );
    SIZE_T name_length = wcslen( name );
    BOOL separator = directory_length && directory[directory_length - 1] != '\\' &&
                     directory[directory_length - 1] != '/';

    if (directory_length + separator + name_length + 1 > capacity)
        return FALSE;

    memcpy( path, directory, directory_length * sizeof(*path) );
    if (separator) path[directory_length++] = '\\';
    memcpy( path + directory_length, name, (name_length + 1) * sizeof(*path) );
    return TRUE;
}

static BOOL regular_file_exists( const WCHAR *path )
{
    DWORD attributes = GetFileAttributesW( path );

    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static HRESULT find_game_config( WCHAR *path, SIZE_T capacity )
{
    static const WCHAR *const names[] =
    {
        L"MicrosoftGame.Config",
        L"MicrosoftGame.config",
    };
    WCHAR directory[MAX_PATH], *separator;
    DWORD length;
    SIZE_T i;

    length = GetModuleFileNameW( NULL, directory, ARRAY_SIZE(directory) );
    if (!length) return HRESULT_FROM_WIN32( GetLastError() );
    if (length >= ARRAY_SIZE(directory))
        return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );

    separator = last_path_separator( directory );
    if (!separator) return E_GAME_MISSING_GAME_CONFIG;
    *separator = 0;

    for (;;)
    {
        for (i = 0; i < ARRAY_SIZE(names); ++i)
        {
            if (!build_config_path( directory, names[i], path, capacity ))
                return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            if (regular_file_exists( path )) return S_OK;
        }

        length = wcslen( directory );
        if (length == 3 && directory[1] == ':' &&
            (directory[2] == '\\' || directory[2] == '/'))
            break;

        separator = last_path_separator( directory );
        if (!separator || separator == directory) break;
        if (separator == directory + 2 && directory[1] == ':')
            separator[1] = 0;
        else
            *separator = 0;
    }

    path[0] = 0;
    return E_GAME_MISSING_GAME_CONFIG;
}

static HRESULT read_game_config( const WCHAR *path, char **contents )
{
    LARGE_INTEGER size;
    HANDLE file;
    DWORD file_size, read, error;
    char *buffer;

    *contents = NULL;
    file = CreateFileW( path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32( GetLastError() );

    if (!GetFileSizeEx( file, &size ))
    {
        error = GetLastError();
        CloseHandle( file );
        return HRESULT_FROM_WIN32( error );
    }
    if (size.QuadPart <= 0 || size.QuadPart > GAME_CONFIG_MAX_SIZE)
    {
        CloseHandle( file );
        return E_GAMERUNTIME_GAMECONFIG_BAD_FORMAT;
    }

    file_size = size.QuadPart;
    buffer = HeapAlloc( GetProcessHeap(), 0, file_size + 1 );
    if (!buffer)
    {
        CloseHandle( file );
        return E_OUTOFMEMORY;
    }

    if (!ReadFile( file, buffer, file_size, &read, NULL ))
    {
        error = GetLastError();
        HeapFree( GetProcessHeap(), 0, buffer );
        CloseHandle( file );
        return HRESULT_FROM_WIN32( error );
    }
    if (read != file_size)
    {
        HeapFree( GetProcessHeap(), 0, buffer );
        CloseHandle( file );
        return HRESULT_FROM_WIN32( ERROR_HANDLE_EOF );
    }
    CloseHandle( file );

    if (memchr( buffer, 0, read ))
    {
        HeapFree( GetProcessHeap(), 0, buffer );
        return E_GAMERUNTIME_GAMECONFIG_BAD_FORMAT;
    }
    buffer[read] = 0;
    *contents = buffer;
    return S_OK;
}

static const char *skip_xml_misc( const char *cursor )
{
    const char *end;

    if (strlen( cursor ) >= 3 &&
        (unsigned char)cursor[0] == 0xef &&
        (unsigned char)cursor[1] == 0xbb &&
        (unsigned char)cursor[2] == 0xbf)
        cursor += 3;

    for (;;)
    {
        while (ascii_space( *cursor )) ++cursor;

        if (!strncmp( cursor, "<?", 2 ))
        {
            if (!(end = strstr( cursor + 2, "?>" ))) return NULL;
            cursor = end + 2;
            continue;
        }
        if (!strncmp( cursor, "<!--", 4 ))
        {
            if (!(end = strstr( cursor + 4, "-->" ))) return NULL;
            cursor = end + 3;
            continue;
        }
        return cursor;
    }
}

/* Returns 1 for a found element, 0 when absent, and -1 for malformed XML. */
static int get_element_text( const char *root, const char *root_end,
                             const char *open_tag, const char *close_tag,
                             const char **value, SIZE_T *length )
{
    SIZE_T open_length = strlen( open_tag );
    const char *cursor = root, *open_end, *close, *attribute;

    while ((cursor = strstr( cursor, open_tag )) && cursor < root_end)
    {
        if (cursor[open_length] != '>' && !ascii_space( cursor[open_length] ))
        {
            cursor += open_length;
            continue;
        }

        open_end = strchr( cursor + open_length, '>' );
        if (!open_end || open_end >= root_end ||
            (open_end > cursor && open_end[-1] == '/'))
            return -1;
        for (attribute = cursor + open_length; attribute < open_end; ++attribute)
            if (!ascii_space( *attribute )) return -1;
        close = strstr( open_end + 1, close_tag );
        if (!close || close >= root_end) return -1;

        *value = open_end + 1;
        *length = close - *value;
        while (*length && ascii_space( **value ))
        {
            ++*value;
            --*length;
        }
        while (*length && ascii_space( (*value)[*length - 1] )) --*length;
        return 1;
    }

    return 0;
}

static int hex_value( char value )
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static BOOL parse_title_id( const char *text, SIZE_T length, UINT32 *title_id )
{
    UINT32 value = 0;
    SIZE_T i;
    int digit;

    /* MicrosoftGame.Config encodes TitleId as exactly eight hexadecimal
     * digits.  Parsing it as decimal silently turns 35760C07 into 35760. */
    if (length != 8) return FALSE;
    for (i = 0; i < length; ++i)
    {
        if ((digit = hex_value( text[i] )) < 0) return FALSE;
        value = (value << 4) | digit;
    }
    if (!value) return FALSE;

    *title_id = value;
    return TRUE;
}

static BOOL parse_msa_app_id( const char *text, SIZE_T length, char app_id[17] )
{
    SIZE_T i;

    if (length != 16) return FALSE;
    for (i = 0; i < length; ++i)
        if (hex_value( text[i] ) < 0) return FALSE;

    memcpy( app_id, text, length );
    app_id[length] = 0;
    return TRUE;
}

static BOOL text_equals( const char *text, SIZE_T length, const char *expected )
{
    return length == strlen( expected ) && !memcmp( text, expected, length );
}

static HRESULT parse_game_config( const char *contents )
{
    char msa_app_id[17] = {0};
    const char *root, *root_open_end, *root_end, *value;
    UINT32 title_id = 0;
    BOOLEAN full_trust = FALSE;
    SIZE_T length;
    int found, has_title, has_msa;

    if (!(root = skip_xml_misc( contents ))) goto malformed;
    if (strncmp( root, "<Game", 5 ) ||
        (root[5] != '>' && !ascii_space( root[5] )))
        goto malformed;
    if (!(root_open_end = strchr( root + 5, '>' )) ||
        !(root_end = strstr( root_open_end + 1, "</Game>" )))
        goto malformed;

    has_title = get_element_text( root_open_end + 1, root_end,
                                  "<TitleId", "</TitleId>", &value, &length );
    if (has_title < 0 ||
        (has_title && !parse_title_id( value, length, &title_id )))
        goto malformed;

    has_msa = get_element_text( root_open_end + 1, root_end,
                                "<MSAAppId", "</MSAAppId>", &value, &length );
    if (has_msa < 0 ||
        (has_msa && !parse_msa_app_id( value, length, msa_app_id )))
        goto malformed;
    if (!!has_title != !!has_msa) goto malformed;

    found = get_element_text( root_open_end + 1, root_end, "<MSAFullTrust",
                              "</MSAFullTrust>", &value, &length );
    if (found < 0) goto malformed;
    if (found)
    {
        if (text_equals( value, length, "true" ) ||
            text_equals( value, length, "1" ))
            full_trust = TRUE;
        else if (!text_equals( value, length, "false" ) &&
                 !text_equals( value, length, "0" ))
            goto malformed;
    }

    winegdk_game_title_id = title_id;
    memcpy( winegdk_game_msa_app_id, msa_app_id,
            sizeof(winegdk_game_msa_app_id) );
    winegdk_game_msa_full_trust = full_trust;
    return S_OK;

malformed:
    return E_GAMERUNTIME_GAMECONFIG_BAD_FORMAT;
}

static BOOL CALLBACK load_game_config_once( INIT_ONCE *once, void *parameter,
                                            void **context )
{
    WCHAR path[MAX_PATH] = {0};
    char *contents = NULL;
    HRESULT status;

    (void)once;
    (void)parameter;
    (void)context;

    status = find_game_config( path, ARRAY_SIZE(path) );
    if (SUCCEEDED( status )) status = read_game_config( path, &contents );
    if (SUCCEEDED( status )) status = parse_game_config( contents );

    if (contents) HeapFree( GetProcessHeap(), 0, contents );
    game_config_status = status;

    if (SUCCEEDED( status ))
        ERR( "native XGame identity loaded: TitleId 0x%08x.\n",
             winegdk_game_title_id );
    else if (status == E_GAME_MISSING_GAME_CONFIG)
        WARN( "MicrosoftGame.Config not found next to the executable or its parents.\n" );
    else
        ERR( "could not load MicrosoftGame.Config, hr %#lx.\n", status );

    return TRUE;
}

HRESULT WineGDKLoadGameConfig( void )
{
    if (!InitOnceExecuteOnce( &game_config_once, load_game_config_once, NULL, NULL ))
        return HRESULT_FROM_WIN32( GetLastError() );
    return game_config_status;
}

static HRESULT WINAPI x_game_QueryInterface( IXGameImpl3 *iface, REFIID iid,
                                              void **out )
{
    struct x_game *impl = impl_from_IXGameImpl3( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IXGameImpl ) ||
        IsEqualGUID( iid, &IID_IXGameImpl2 ) ||
        IsEqualGUID( iid, &IID_IXGameImpl3 ))
    {
        *out = &impl->IXGameImpl3_iface;
        IXGameImpl3_AddRef( &impl->IXGameImpl3_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI x_game_AddRef( IXGameImpl3 *iface )
{
    struct x_game *impl = impl_from_IXGameImpl3( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );

    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_game_Release( IXGameImpl3 *iface )
{
    struct x_game *impl = impl_from_IXGameImpl3( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_game_XGameGetXboxTitleId( IXGameImpl3 *iface,
                                                   UINT32 *value )
{
    HRESULT status;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = 0;
    status = WineGDKLoadGameConfig();
    if (FAILED( status ) && status != E_GAME_MISSING_GAME_CONFIG)
        return status;
    if (!winegdk_game_title_id)
        return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );

    *value = winegdk_game_title_id;
    return S_OK;
}

static void WINAPI x_game_XLaunchNewGame( IXGameImpl3 *iface,
                                          const char *exe_path, const char *args,
                                          XUserHandle default_user )
{
    FIXME( "iface %p, exe_path %p, args %p, default_user %p stub!\n", iface,
           exe_path, args, default_user );
}

static HRESULT WINAPI x_game_XLaunchRestartOnCrash( IXGameImpl3 *iface,
                                                     const char *args,
                                                     UINT32 reserved )
{
    FIXME( "iface %p, args %p, reserved %u stub!\n", iface, args, reserved );
    return E_NOTIMPL;
}

static const struct IXGameImpl3Vtbl x_game_vtbl =
{
    x_game_QueryInterface,
    x_game_AddRef,
    x_game_Release,
    /* IXGameImpl methods */
    x_game_XGameGetXboxTitleId,
    /* IXGameImpl2 methods */
    x_game_XLaunchNewGame,
    /* IXGameImpl3 methods */
    x_game_XLaunchRestartOnCrash,
};

static struct x_game x_game =
{
    {&x_game_vtbl},
    0,
};

IXGameImpl *x_game_impl = (IXGameImpl *)&x_game.IXGameImpl3_iface;
