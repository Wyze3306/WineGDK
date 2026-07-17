/*
 * Copyright (C) 2025 Mohamad Al-Jaf
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
#define COBJMACROS
#include "initguid.h"
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winstring.h"

#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Storage_Streams
#include "windows.storage.streams.h"
#define WIDL_using_Microsoft_UI
#define WIDL_using_Microsoft_Windows_Storage_Pickers
#include "microsoft.ui.h"
#include "microsoft.windows.storage.pickers.h"

#include "wine/test.h"

#define check_interface( obj, iid, is_broken ) check_interface_( __LINE__, obj, iid, is_broken )
static void check_interface_( unsigned int line, void *obj, const IID *iid, BOOL is_broken )
{
    IUnknown *iface = obj;
    IUnknown *unk;
    HRESULT hr;

    hr = IUnknown_QueryInterface( iface, iid, (void **)&unk );
    ok_(__FILE__, line)( hr == S_OK || broken( is_broken && hr == E_NOINTERFACE ), "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
        IUnknown_Release( unk );
}

static void test_RandomAccessStreamReference(void)
{
    static const WCHAR *random_access_stream_reference_statics_name = L"Windows.Storage.Streams.RandomAccessStreamReference";
    IRandomAccessStreamReferenceStatics *random_access_stream_reference_statics = (void *)0xdeadbeef;
    IRandomAccessStreamReference *random_access_stream_reference = (void *)0xdeadbeef;
    IActivationFactory *factory = (void *)0xdeadbeef;
    HSTRING str = NULL;
    HRESULT hr;
    LONG ref;

    hr = WindowsCreateString( random_access_stream_reference_statics_name, wcslen( random_access_stream_reference_statics_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( random_access_stream_reference_statics_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown, FALSE );
    check_interface( factory, &IID_IInspectable, FALSE );
    check_interface( factory, &IID_IAgileObject, TRUE /* Supported after Windows 10 1607 */ );

    hr = IActivationFactory_QueryInterface( factory, &IID_IRandomAccessStreamReferenceStatics, (void **)&random_access_stream_reference_statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = IRandomAccessStreamReferenceStatics_CreateFromStream( random_access_stream_reference_statics, NULL, &random_access_stream_reference );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    ok( random_access_stream_reference == NULL, "IRandomAccessStreamReferenceStatics_CreateFromStream returned %p.\n", random_access_stream_reference );

    ref = IRandomAccessStreamReferenceStatics_Release( random_access_stream_reference_statics );
    ok( ref == 1, "got ref %ld.\n", ref );
    ref = IActivationFactory_Release( factory );
    ok( ref == 0, "got ref %ld.\n", ref );
}

static void test_FileOpenPicker(void)
{
    static const WCHAR class_name[] = L"Microsoft.Windows.Storage.Pickers.FileOpenPicker";
    static const IID async_iid = {0x98316bd9, 0x5a80, 0x5c8a, {0x8f, 0x99, 0x2e, 0xfb, 0xe1, 0xe7, 0x02, 0x33}};
    static const IID handler_iid = {0x4689da0a, 0xd31d, 0x5be4, {0x87, 0x22, 0xbf, 0xdb, 0x60, 0x89, 0x30, 0x26}};
    IFileOpenPickerFactory *picker_factory = (void *)0xdeadbeef;
    IFileOpenPicker *picker = (void *)0xdeadbeef;
    IActivationFactory *activation_factory;
    IIterable_HSTRING *iterable = NULL;
    IIterator_HSTRING *iterator = NULL;
    IVector_HSTRING *mutable_view = (void *)0xdeadbeef;
    IVectorView_HSTRING *view = NULL;
    IVector_HSTRING *filters = NULL;
    PickerLocationId location;
    PickerViewMode view_mode;
    WindowId window_id = {0};
    HSTRING value = NULL, copy = NULL, item = NULL, name = NULL;
    boolean has_current;
    UINT32 count, size;
    HRESULT hr;

    ok( IsEqualGUID( &IID_IAsyncOperation_PickFileResult, &async_iid ), "unexpected async operation IID.\n" );
    ok( IsEqualGUID( &IID_IAsyncOperationCompletedHandler_PickFileResult, &handler_iid ),
        "unexpected completion handler IID.\n" );

    hr = WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, &name );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( name, &IID_IFileOpenPickerFactory, (void **)&picker_factory );
    WindowsDeleteString( name );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w(class_name) );
        return;
    }
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;

    check_interface( picker_factory, &IID_IUnknown, FALSE );
    check_interface( picker_factory, &IID_IInspectable, FALSE );
    check_interface( picker_factory, &IID_IAgileObject, FALSE );
    check_interface( picker_factory, &IID_IActivationFactory, FALSE );

    hr = IFileOpenPickerFactory_QueryInterface( picker_factory, &IID_IActivationFactory,
                                                (void **)&activation_factory );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        IInspectable *instance = (void *)0xdeadbeef;
        hr = IActivationFactory_ActivateInstance( activation_factory, &instance );
        ok( hr == E_NOTIMPL, "got hr %#lx.\n", hr );
        ok( instance == NULL, "got instance %p.\n", instance );
        IActivationFactory_Release( activation_factory );
    }

    hr = IFileOpenPickerFactory_CreateInstance( picker_factory, window_id, &picker );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr))
    {
        IFileOpenPickerFactory_Release( picker_factory );
        return;
    }

    check_interface( picker, &IID_IUnknown, FALSE );
    check_interface( picker, &IID_IInspectable, FALSE );
    check_interface( picker, &IID_IAgileObject, FALSE );

    hr = IFileOpenPicker_get_ViewMode( picker, &view_mode );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( view_mode == PickerViewMode_List, "got mode %d.\n", view_mode );
    hr = IFileOpenPicker_put_ViewMode( picker, PickerViewMode_Thumbnail );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IFileOpenPicker_put_ViewMode( picker, 42 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    hr = IFileOpenPicker_get_SuggestedStartLocation( picker, &location );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( location == PickerLocationId_Unspecified, "got location %d.\n", location );
    hr = IFileOpenPicker_put_SuggestedStartLocation( picker, PickerLocationId_Downloads );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IFileOpenPicker_put_SuggestedStartLocation( picker, 4 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    hr = WindowsCreateString( L"Import", 6, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IFileOpenPicker_put_CommitButtonText( picker, value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IFileOpenPicker_get_CommitButtonText( picker, &copy );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !wcscmp( WindowsGetStringRawBuffer( copy, NULL ), L"Import" ), "got %s.\n", debugstr_hstring(copy) );
    WindowsDeleteString( copy );
    WindowsDeleteString( value );

    hr = IFileOpenPicker_get_FileTypeFilter( picker, &filters );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = WindowsCreateString( L"mcworld", 7, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IVector_HSTRING_Append( filters, value );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = IVector_HSTRING_SetAt( filters, 100, value );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = IVector_HSTRING_InsertAt( filters, 100, value );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    WindowsDeleteString( value );
    hr = WindowsCreateString( L".mcworld", 8, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IVector_HSTRING_Append( filters, value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( value );
    hr = IVector_HSTRING_QueryInterface( filters, &IID_IIterable_HSTRING, (void **)&iterable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IIterable_HSTRING_First( iterable, &iterator );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IIterable_HSTRING_Release( iterable );
    hr = IVector_HSTRING_GetView( filters, &view );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = WindowsCreateString( L".png", 4, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IVector_HSTRING_Append( filters, value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( value );
    hr = IVector_HSTRING_get_Size( filters, &size );
    ok( hr == S_OK && size == 2, "got hr %#lx, size %u.\n", hr, size );
    hr = IVectorView_HSTRING_get_Size( view, &size );
    ok( hr == S_OK && size == 2, "got hr %#lx, live view size %u.\n", hr, size );
    hr = IVectorView_HSTRING_QueryInterface( view, &IID_IVector_HSTRING, (void **)&mutable_view );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) IVector_HSTRING_Release( mutable_view );

    hr = IIterator_HSTRING_get_Current( iterator, &item );
    ok( hr == E_CHANGED_STATE, "got hr %#lx.\n", hr );
    ok( item == NULL, "got item %s.\n", debugstr_hstring(item) );
    has_current = TRUE;
    hr = IIterator_HSTRING_get_HasCurrent( iterator, &has_current );
    ok( hr == E_CHANGED_STATE, "got hr %#lx.\n", hr );
    ok( !has_current, "got HasCurrent %u.\n", has_current );
    has_current = TRUE;
    hr = IIterator_HSTRING_MoveNext( iterator, &has_current );
    ok( hr == E_CHANGED_STATE, "got hr %#lx.\n", hr );
    ok( !has_current, "got HasCurrent %u.\n", has_current );
    count = 0xdeadbeef;
    hr = IIterator_HSTRING_GetMany( iterator, 1, &item, &count );
    ok( hr == E_CHANGED_STATE, "got hr %#lx.\n", hr );
    ok( !count, "got count %u.\n", count );

    count = 0xdeadbeef;
    hr = IVector_HSTRING_GetMany( filters, size, 1, &item, &count );
    ok( hr == S_OK && !count, "got hr %#lx, count %u.\n", hr, count );
    count = 0xdeadbeef;
    hr = IVector_HSTRING_GetMany( filters, size + 1, 1, &item, &count );
    ok( hr == S_OK && !count, "got hr %#lx, count %u.\n", hr, count );

    IIterator_HSTRING_Release( iterator );
    IVectorView_HSTRING_Release( view );
    IVector_HSTRING_Release( filters );
    IFileOpenPicker_Release( picker );
    IFileOpenPickerFactory_Release( picker_factory );
}

START_TEST(storage)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_RandomAccessStreamReference();
    test_FileOpenPicker();

    RoUninitialize();
}
