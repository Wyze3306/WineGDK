/* WinRT HSTRING vector used by the storage pickers.
 *
 * Copyright 2026 BedrockOnLinux contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "private.h"

struct hstring_vector
{
    IVector_HSTRING IVector_HSTRING_iface;
    IVectorView_HSTRING IVectorView_HSTRING_iface;
    IIterable_HSTRING IIterable_HSTRING_iface;
    LONG ref;
    LONG version;
    UINT32 size;
    UINT32 capacity;
    HSTRING *items;
};

struct hstring_iterator
{
    IIterator_HSTRING IIterator_HSTRING_iface;
    LONG ref;
    struct hstring_vector *vector;
    LONG version;
    UINT32 index;
};

static const IVector_HSTRINGVtbl vector_vtbl;
static const IVectorView_HSTRINGVtbl view_vtbl;
static const IIterable_HSTRINGVtbl iterable_vtbl;

static inline struct hstring_vector *vector_from_IVector( IVector_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_vector, IVector_HSTRING_iface );
}

static inline struct hstring_vector *vector_from_IVectorView( IVectorView_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_vector, IVectorView_HSTRING_iface );
}

static inline struct hstring_vector *vector_from_IIterable( IIterable_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_vector, IIterable_HSTRING_iface );
}

static inline struct hstring_iterator *iterator_from_IIterator( IIterator_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_iterator, IIterator_HSTRING_iface );
}

static HRESULT vector_query_interface( struct hstring_vector *vector, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IVector_HSTRING ))
        *out = &vector->IVector_HSTRING_iface;
    else if (IsEqualGUID( iid, &IID_IVectorView_HSTRING ))
        *out = &vector->IVectorView_HSTRING_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_HSTRING ))
        *out = &vector->IIterable_HSTRING_iface;
    else
        return E_NOINTERFACE;

    InterlockedIncrement( &vector->ref );
    return S_OK;
}

static ULONG vector_add_ref( struct hstring_vector *vector )
{
    return InterlockedIncrement( &vector->ref );
}

static LONG vector_get_version( struct hstring_vector *vector )
{
    return InterlockedCompareExchange( &vector->version, 0, 0 );
}

static void vector_increment_version( struct hstring_vector *vector )
{
    InterlockedIncrement( &vector->version );
}

static ULONG vector_release( struct hstring_vector *vector )
{
    ULONG ref = InterlockedDecrement( &vector->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < vector->size; ++i) WindowsDeleteString( vector->items[i] );
        free( vector->items );
        free( vector );
    }
    return ref;
}

static HRESULT inspectable_get_iids( ULONG *iid_count, IID **iids, const IID *iid )
{
    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    if (!(*iids = CoTaskMemAlloc( sizeof(**iids) ))) return E_OUTOFMEMORY;
    **iids = *iid;
    *iid_count = 1;
    return S_OK;
}

static HRESULT inspectable_get_class_name( HSTRING *class_name )
{
    if (!class_name) return E_POINTER;
    *class_name = NULL;
    return S_OK;
}

static HRESULT inspectable_get_trust_level( TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT validate_filter( HSTRING value )
{
    const WCHAR *buffer;
    UINT32 length, i;

    buffer = WindowsGetStringRawBuffer( value, &length );
    if (length == 1 && buffer[0] == '*') return S_OK;
    if (!length || buffer[0] != '.') return E_INVALIDARG;
    for (i = 1; i < length; ++i)
        if (!buffer[i] || buffer[i] == '.' || buffer[i] == '*' || buffer[i] == '?') return E_INVALIDARG;
    return S_OK;
}

static HRESULT vector_get_at( struct hstring_vector *vector, UINT32 index, HSTRING *value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    if (index >= vector->size) return E_BOUNDS;
    return WindowsDuplicateString( vector->items[index], value );
}

static HRESULT vector_get_size( struct hstring_vector *vector, UINT32 *value )
{
    if (!value) return E_POINTER;
    *value = vector->size;
    return S_OK;
}

static HRESULT vector_index_of( struct hstring_vector *vector, HSTRING value, UINT32 *index, boolean *found )
{
    const WCHAR *needle, *item;
    UINT32 needle_len, item_len, i;

    if (!index || !found) return E_POINTER;
    needle = WindowsGetStringRawBuffer( value, &needle_len );
    for (i = 0; i < vector->size; ++i)
    {
        item = WindowsGetStringRawBuffer( vector->items[i], &item_len );
        if (needle_len == item_len && !memcmp( needle, item, needle_len * sizeof(WCHAR) )) break;
    }
    *found = i < vector->size;
    *index = *found ? i : 0;
    return S_OK;
}

static HRESULT WINAPI vector_QueryInterface( IVector_HSTRING *iface, REFIID iid, void **out )
{
    return vector_query_interface( vector_from_IVector( iface ), iid, out );
}

static ULONG WINAPI vector_AddRef( IVector_HSTRING *iface )
{
    return vector_add_ref( vector_from_IVector( iface ) );
}

static ULONG WINAPI vector_Release( IVector_HSTRING *iface )
{
    return vector_release( vector_from_IVector( iface ) );
}

static HRESULT WINAPI vector_GetIids( IVector_HSTRING *iface, ULONG *count, IID **iids )
{
    return inspectable_get_iids( count, iids, &IID_IVector_HSTRING );
}

static HRESULT WINAPI vector_GetRuntimeClassName( IVector_HSTRING *iface, HSTRING *name )
{
    return inspectable_get_class_name( name );
}

static HRESULT WINAPI vector_GetTrustLevel( IVector_HSTRING *iface, TrustLevel *level )
{
    return inspectable_get_trust_level( level );
}

static HRESULT WINAPI vector_GetAt( IVector_HSTRING *iface, UINT32 index, HSTRING *value )
{
    return vector_get_at( vector_from_IVector( iface ), index, value );
}

static HRESULT WINAPI vector_get_Size( IVector_HSTRING *iface, UINT32 *value )
{
    return vector_get_size( vector_from_IVector( iface ), value );
}

static HRESULT WINAPI vector_GetView( IVector_HSTRING *iface, IVectorView_HSTRING **value )
{
    struct hstring_vector *vector = vector_from_IVector( iface );

    if (!value) return E_POINTER;
    IVectorView_HSTRING_AddRef( *value = &vector->IVectorView_HSTRING_iface );
    return S_OK;
}

static HRESULT WINAPI vector_IndexOf( IVector_HSTRING *iface, HSTRING value, UINT32 *index, boolean *found )
{
    return vector_index_of( vector_from_IVector( iface ), value, index, found );
}

static HRESULT WINAPI vector_SetAt( IVector_HSTRING *iface, UINT32 index, HSTRING value )
{
    struct hstring_vector *vector = vector_from_IVector( iface );
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = validate_filter( value ))) return hr;
    if (index >= vector->size) return E_BOUNDS;
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( vector->items[index] );
    vector->items[index] = copy;
    vector_increment_version( vector );
    return S_OK;
}

static HRESULT WINAPI vector_InsertAt( IVector_HSTRING *iface, UINT32 index, HSTRING value )
{
    struct hstring_vector *vector = vector_from_IVector( iface );
    HSTRING *items, copy;
    UINT32 capacity;
    HRESULT hr;

    if (FAILED(hr = validate_filter( value ))) return hr;
    if (index > vector->size) return E_BOUNDS;
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;

    if (vector->size == vector->capacity)
    {
        capacity = vector->capacity ? vector->capacity * 2 : 8;
        if (capacity < vector->capacity || !(items = realloc( vector->items, capacity * sizeof(*items) )))
        {
            WindowsDeleteString( copy );
            return E_OUTOFMEMORY;
        }
        vector->items = items;
        vector->capacity = capacity;
    }

    memmove( vector->items + index + 1, vector->items + index,
             (vector->size - index) * sizeof(*vector->items) );
    vector->items[index] = copy;
    ++vector->size;
    vector_increment_version( vector );
    return S_OK;
}

static HRESULT WINAPI vector_RemoveAt( IVector_HSTRING *iface, UINT32 index )
{
    struct hstring_vector *vector = vector_from_IVector( iface );
    if (index >= vector->size) return E_BOUNDS;
    WindowsDeleteString( vector->items[index] );
    --vector->size;
    memmove( vector->items + index, vector->items + index + 1,
             (vector->size - index) * sizeof(*vector->items) );
    vector_increment_version( vector );
    return S_OK;
}

static HRESULT WINAPI vector_Append( IVector_HSTRING *iface, HSTRING value )
{
    struct hstring_vector *vector = vector_from_IVector( iface );
    return vector_InsertAt( iface, vector->size, value );
}

static HRESULT WINAPI vector_RemoveAtEnd( IVector_HSTRING *iface )
{
    struct hstring_vector *vector = vector_from_IVector( iface );
    if (!vector->size) return E_BOUNDS;
    return vector_RemoveAt( iface, vector->size - 1 );
}

static HRESULT WINAPI vector_Clear( IVector_HSTRING *iface )
{
    struct hstring_vector *vector = vector_from_IVector( iface );
    UINT32 i;
    for (i = 0; i < vector->size; ++i) WindowsDeleteString( vector->items[i] );
    vector->size = 0;
    vector_increment_version( vector );
    return S_OK;
}

static HRESULT WINAPI vector_GetMany( IVector_HSTRING *iface, UINT32 start, UINT32 capacity,
                                      HSTRING *items, UINT32 *count )
{
    struct hstring_vector *vector = vector_from_IVector( iface );
    UINT32 i, available;
    HRESULT hr;

    if (!count || (capacity && !items)) return E_POINTER;
    *count = 0;
    if (start >= vector->size) return S_OK;
    available = min( capacity, vector->size - start );
    for (i = 0; i < available; ++i)
    {
        if (FAILED(hr = WindowsDuplicateString( vector->items[start + i], &items[i] )))
        {
            while (i) WindowsDeleteString( items[--i] );
            return hr;
        }
    }
    *count = available;
    return S_OK;
}

static HRESULT WINAPI vector_ReplaceAll( IVector_HSTRING *iface, UINT32 count, HSTRING *items )
{
    struct hstring_vector *vector = vector_from_IVector( iface );
    HSTRING *copies = NULL;
    UINT32 i;
    HRESULT hr;

    if (count && !items) return E_POINTER;
    if (count && !(copies = calloc( count, sizeof(*copies) ))) return E_OUTOFMEMORY;
    for (i = 0; i < count; ++i)
    {
        if (FAILED(hr = validate_filter( items[i] )) ||
            FAILED(hr = WindowsDuplicateString( items[i], &copies[i] )))
        {
            while (i) WindowsDeleteString( copies[--i] );
            free( copies );
            return hr;
        }
    }
    for (i = 0; i < vector->size; ++i) WindowsDeleteString( vector->items[i] );
    free( vector->items );
    vector->items = copies;
    vector->size = vector->capacity = count;
    vector_increment_version( vector );
    return S_OK;
}

static const IVector_HSTRINGVtbl vector_vtbl =
{
    vector_QueryInterface,
    vector_AddRef,
    vector_Release,
    vector_GetIids,
    vector_GetRuntimeClassName,
    vector_GetTrustLevel,
    vector_GetAt,
    vector_get_Size,
    vector_GetView,
    vector_IndexOf,
    vector_SetAt,
    vector_InsertAt,
    vector_RemoveAt,
    vector_Append,
    vector_RemoveAtEnd,
    vector_Clear,
    vector_GetMany,
    vector_ReplaceAll,
};

static HRESULT WINAPI view_QueryInterface( IVectorView_HSTRING *iface, REFIID iid, void **out )
{
    return vector_query_interface( vector_from_IVectorView( iface ), iid, out );
}

static ULONG WINAPI view_AddRef( IVectorView_HSTRING *iface )
{
    return vector_add_ref( vector_from_IVectorView( iface ) );
}

static ULONG WINAPI view_Release( IVectorView_HSTRING *iface )
{
    return vector_release( vector_from_IVectorView( iface ) );
}

static HRESULT WINAPI view_GetIids( IVectorView_HSTRING *iface, ULONG *count, IID **iids )
{
    return inspectable_get_iids( count, iids, &IID_IVectorView_HSTRING );
}

static HRESULT WINAPI view_GetRuntimeClassName( IVectorView_HSTRING *iface, HSTRING *name )
{
    return inspectable_get_class_name( name );
}

static HRESULT WINAPI view_GetTrustLevel( IVectorView_HSTRING *iface, TrustLevel *level )
{
    return inspectable_get_trust_level( level );
}

static HRESULT WINAPI view_GetAt( IVectorView_HSTRING *iface, UINT32 index, HSTRING *value )
{
    return vector_get_at( vector_from_IVectorView( iface ), index, value );
}

static HRESULT WINAPI view_get_Size( IVectorView_HSTRING *iface, UINT32 *value )
{
    return vector_get_size( vector_from_IVectorView( iface ), value );
}

static HRESULT WINAPI view_IndexOf( IVectorView_HSTRING *iface, HSTRING value, UINT32 *index, boolean *found )
{
    return vector_index_of( vector_from_IVectorView( iface ), value, index, found );
}

static HRESULT WINAPI view_GetMany( IVectorView_HSTRING *iface, UINT32 start, UINT32 capacity,
                                    HSTRING *items, UINT32 *count )
{
    struct hstring_vector *vector = vector_from_IVectorView( iface );
    return vector_GetMany( &vector->IVector_HSTRING_iface, start, capacity, items, count );
}

static const IVectorView_HSTRINGVtbl view_vtbl =
{
    view_QueryInterface,
    view_AddRef,
    view_Release,
    view_GetIids,
    view_GetRuntimeClassName,
    view_GetTrustLevel,
    view_GetAt,
    view_get_Size,
    view_IndexOf,
    view_GetMany,
};

static HRESULT WINAPI iterator_QueryInterface( IIterator_HSTRING *iface, REFIID iid, void **out )
{
    struct hstring_iterator *iterator = iterator_from_IIterator( iface );
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualGUID( iid, &IID_IUnknown ) && !IsEqualGUID( iid, &IID_IInspectable ) &&
        !IsEqualGUID( iid, &IID_IAgileObject ) && !IsEqualGUID( iid, &IID_IIterator_HSTRING ))
        return E_NOINTERFACE;
    InterlockedIncrement( &iterator->ref );
    *out = iface;
    return S_OK;
}

static ULONG WINAPI iterator_AddRef( IIterator_HSTRING *iface )
{
    return InterlockedIncrement( &iterator_from_IIterator( iface )->ref );
}

static ULONG WINAPI iterator_Release( IIterator_HSTRING *iface )
{
    struct hstring_iterator *iterator = iterator_from_IIterator( iface );
    ULONG ref = InterlockedDecrement( &iterator->ref );
    if (!ref)
    {
        vector_release( iterator->vector );
        free( iterator );
    }
    return ref;
}

static HRESULT WINAPI iterator_GetIids( IIterator_HSTRING *iface, ULONG *count, IID **iids )
{
    return inspectable_get_iids( count, iids, &IID_IIterator_HSTRING );
}

static HRESULT WINAPI iterator_GetRuntimeClassName( IIterator_HSTRING *iface, HSTRING *name )
{
    return inspectable_get_class_name( name );
}

static HRESULT WINAPI iterator_GetTrustLevel( IIterator_HSTRING *iface, TrustLevel *level )
{
    return inspectable_get_trust_level( level );
}

static HRESULT WINAPI iterator_get_Current( IIterator_HSTRING *iface, HSTRING *value )
{
    struct hstring_iterator *iterator = iterator_from_IIterator( iface );
    if (!value) return E_POINTER;
    *value = NULL;
    if (iterator->version != vector_get_version( iterator->vector )) return E_CHANGED_STATE;
    return vector_get_at( iterator->vector, iterator->index, value );
}

static HRESULT WINAPI iterator_get_HasCurrent( IIterator_HSTRING *iface, boolean *value )
{
    struct hstring_iterator *iterator = iterator_from_IIterator( iface );
    if (!value) return E_POINTER;
    *value = FALSE;
    if (iterator->version != vector_get_version( iterator->vector )) return E_CHANGED_STATE;
    *value = iterator->index < iterator->vector->size;
    return S_OK;
}

static HRESULT WINAPI iterator_MoveNext( IIterator_HSTRING *iface, boolean *value )
{
    struct hstring_iterator *iterator = iterator_from_IIterator( iface );
    if (!value) return E_POINTER;
    *value = FALSE;
    if (iterator->version != vector_get_version( iterator->vector )) return E_CHANGED_STATE;
    if (iterator->index < iterator->vector->size) ++iterator->index;
    return iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI iterator_GetMany( IIterator_HSTRING *iface, UINT32 capacity,
                                        HSTRING *items, UINT32 *count )
{
    struct hstring_iterator *iterator = iterator_from_IIterator( iface );
    HRESULT hr;

    if (!count || (capacity && !items)) return E_POINTER;
    *count = 0;
    if (iterator->version != vector_get_version( iterator->vector )) return E_CHANGED_STATE;
    hr = vector_GetMany( &iterator->vector->IVector_HSTRING_iface, iterator->index,
                         capacity, items, count );
    if (SUCCEEDED(hr)) iterator->index += *count;
    return hr;
}

static const IIterator_HSTRINGVtbl iterator_vtbl =
{
    iterator_QueryInterface,
    iterator_AddRef,
    iterator_Release,
    iterator_GetIids,
    iterator_GetRuntimeClassName,
    iterator_GetTrustLevel,
    iterator_get_Current,
    iterator_get_HasCurrent,
    iterator_MoveNext,
    iterator_GetMany,
};

static HRESULT WINAPI iterable_QueryInterface( IIterable_HSTRING *iface, REFIID iid, void **out )
{
    return vector_query_interface( vector_from_IIterable( iface ), iid, out );
}

static ULONG WINAPI iterable_AddRef( IIterable_HSTRING *iface )
{
    return vector_add_ref( vector_from_IIterable( iface ) );
}

static ULONG WINAPI iterable_Release( IIterable_HSTRING *iface )
{
    return vector_release( vector_from_IIterable( iface ) );
}

static HRESULT WINAPI iterable_GetIids( IIterable_HSTRING *iface, ULONG *count, IID **iids )
{
    return inspectable_get_iids( count, iids, &IID_IIterable_HSTRING );
}

static HRESULT WINAPI iterable_GetRuntimeClassName( IIterable_HSTRING *iface, HSTRING *name )
{
    return inspectable_get_class_name( name );
}

static HRESULT WINAPI iterable_GetTrustLevel( IIterable_HSTRING *iface, TrustLevel *level )
{
    return inspectable_get_trust_level( level );
}

static HRESULT WINAPI iterable_First( IIterable_HSTRING *iface, IIterator_HSTRING **value )
{
    struct hstring_vector *vector = vector_from_IIterable( iface );
    struct hstring_iterator *iterator;

    if (!value) return E_POINTER;
    *value = NULL;
    if (!(iterator = calloc( 1, sizeof(*iterator) ))) return E_OUTOFMEMORY;
    iterator->IIterator_HSTRING_iface.lpVtbl = &iterator_vtbl;
    iterator->ref = 1;
    vector_add_ref( iterator->vector = vector );
    iterator->version = vector_get_version( vector );
    *value = &iterator->IIterator_HSTRING_iface;
    return S_OK;
}

static const IIterable_HSTRINGVtbl iterable_vtbl =
{
    iterable_QueryInterface,
    iterable_AddRef,
    iterable_Release,
    iterable_GetIids,
    iterable_GetRuntimeClassName,
    iterable_GetTrustLevel,
    iterable_First,
};

HRESULT hstring_vector_create( IVector_HSTRING **out )
{
    struct hstring_vector *vector;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!(vector = calloc( 1, sizeof(*vector) ))) return E_OUTOFMEMORY;
    vector->IVector_HSTRING_iface.lpVtbl = &vector_vtbl;
    vector->IVectorView_HSTRING_iface.lpVtbl = &view_vtbl;
    vector->IIterable_HSTRING_iface.lpVtbl = &iterable_vtbl;
    vector->ref = 1;
    *out = &vector->IVector_HSTRING_iface;
    return S_OK;
}
