/*
 * XGameEvent implementation for {bbfbdcc7-bfe7-409b-a5ca-edf054960b4d}
 *
 * This is the class XGameEventWrite() resolves to, and the only way a GDK
 * title reports gameplay to Xbox: achievements and stats configured as
 * "event based" are graded by the service from these writes, never from a
 * client-side unlock (achievements.xboxlive.com answers a direct update with
 * 400 code 49, "None of the submitted achievements may be updated in this
 * fashion"), so a title running here can never unlock anything while the
 * write is dropped.
 *
 * The class id, the vtable slot and the argument order were read out of
 * Minecraft's own copy of the GDK static library, which reaches this DLL
 * through QueryApiImpl:
 *
 *   XGameEventWrite glue:  lea rcx,[CLSID]; mov rdx,rcx  (class id doubles as
 *                          the interface id, as it does for
 *                          XGameRuntimeFeatureImpl); QueryApiImpl(...);
 *                          call [vtable+0x18]  -> slot 3;
 *                          call [vtable+0x10]  -> Release.
 *
 *   its caller, xbox::services::events::EventsService::WriteInGameEvent,
 *   checks XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature_XGameEvent)
 *   first and passes, in order: the user handle, the service configuration id
 *   lowercased, the play session id, the event name, and the dimensions and
 *   measurements property bags as JSON. All five strings are UTF-8 char *.
 *
 * Nothing is uploaded yet: this reports what the title writes so the events
 * Minecraft actually emits can be read off a session, which is what the
 * ingestion client still to be written has to reproduce.
 */

#include "../../private.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static LONG game_event_ref = 1;

static HRESULT WINAPI game_event_QueryInterface( void *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p\n", iface, debugstr_guid( iid ), out );
    if (!out) return E_POINTER;
    *out = iface;
    InterlockedIncrement( &game_event_ref );
    return S_OK;
}

static ULONG WINAPI game_event_AddRef( void *iface )
{
    return InterlockedIncrement( &game_event_ref );
}

static ULONG WINAPI game_event_Release( void *iface )
{
    return InterlockedDecrement( &game_event_ref );
}

/* The debug channel truncates long strings, and a dimensions bag runs past
 * that in every event Minecraft writes, so the record that has to be complete
 * goes to a file: one JSON object per line, named by BOL_GAME_EVENT_LOG (a
 * Windows path). Unset, nothing is written. */
static void game_event_record( const char *scid, const char *session, const char *name,
                               const char *dimensions, const char *measurements )
{
    static const char *const empty = "";
    char path[MAX_PATH];
    HANDLE file;
    DWORD written;
    size_t size;
    char *line;

    if (!GetEnvironmentVariableA( "BOL_GAME_EVENT_LOG", path, ARRAY_SIZE(path) ))
        return;
    if (!scid) scid = empty;
    if (!session) session = empty;
    if (!name) name = empty;
    if (!dimensions) dimensions = "{}";
    if (!measurements) measurements = "{}";

    size = strlen( scid ) + strlen( session ) + strlen( name ) +
           strlen( dimensions ) + strlen( measurements ) + 128;
    if (!(line = malloc( size ))) return;
    /* The property bags are already JSON, so they go in unquoted; Minecraft
     * ends them with a newline, which would break the one-object-per-line
     * shape and is trimmed below. */
    snprintf( line, size,
              "{\"event\":\"%s\",\"scid\":\"%s\",\"session\":\"%s\","
              "\"dimensions\":%s,\"measurements\":%s}",
              name, scid, session, dimensions, measurements );
    for (char *scan = line; *scan; ++scan)
        if (*scan == '\n' || *scan == '\r') *scan = ' ';

    file = CreateFileA( path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file != INVALID_HANDLE_VALUE)
    {
        WriteFile( file, line, strlen( line ), &written, NULL );
        WriteFile( file, "\n", 1, &written, NULL );
        CloseHandle( file );
    }
    else WARN( "could not open %s for the event record\n", debugstr_a( path ) );
    free( line );
}

/* vtable[3]: XGameEventWrite( user, scid, playSessionId, eventName,
 *                             dimensions, measurements ) */
static HRESULT WINAPI game_event_Write( void *iface, void *user, const char *scid,
                                        const char *play_session, const char *event_name,
                                        const char *dimensions, const char *measurements )
{
    /* ERR, not TRACE: this is the only record of what the title tried to
     * report, and the launcher runs the game with traces off. */
    ERR( "XGameEventWrite user %p, scid %s, session %s, event %s, dimensions %s, measurements %s\n",
         user, debugstr_a( scid ), debugstr_a( play_session ), debugstr_a( event_name ),
         debugstr_a( dimensions ), debugstr_a( measurements ) );

    game_event_record( scid, play_session, event_name, dimensions, measurements );

    /* S_OK is what the title expects; the write is still going nowhere. */
    return S_OK;
}

static const void *game_event_vtable[4] =
{
    game_event_QueryInterface,
    game_event_AddRef,
    game_event_Release,
    game_event_Write,
};

static struct { const void **vtbl; } game_event_instance = { game_event_vtable };

void *x_game_event_get(void)
{
    TRACE( "returning game event implementation %p\n", &game_event_instance );
    return &game_event_instance;
}
