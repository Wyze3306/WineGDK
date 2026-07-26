/* WinRT Windows.Storage.StorageFile implementation.
 *
 * Copyright 2026 BedrockOnLinux contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "private.h"

#define ASYNC_CLOSED ((AsyncStatus)4)

static const IID storage_file_statics_iid =
    {0x5984c710, 0xdaf2, 0x43c8, {0x8b, 0xb4, 0xa4, 0xd3, 0xea, 0xcf, 0xd0, 0x3f}};

typedef struct IStorageFileStatics IStorageFileStatics;

typedef struct IStorageFileStaticsVtbl
{
    HRESULT (WINAPI *QueryInterface)(IStorageFileStatics *, REFIID, void **);
    ULONG (WINAPI *AddRef)(IStorageFileStatics *);
    ULONG (WINAPI *Release)(IStorageFileStatics *);
    HRESULT (WINAPI *GetIids)(IStorageFileStatics *, ULONG *, IID **);
    HRESULT (WINAPI *GetRuntimeClassName)(IStorageFileStatics *, HSTRING *);
    HRESULT (WINAPI *GetTrustLevel)(IStorageFileStatics *, TrustLevel *);
    HRESULT (WINAPI *GetFileFromPathAsync)(IStorageFileStatics *, HSTRING,
                                           IAsyncOperation_StorageFile **);
    HRESULT (WINAPI *GetFileFromApplicationUriAsync)(IStorageFileStatics *, void *,
                                                     IAsyncOperation_StorageFile **);
    HRESULT (WINAPI *CreateStreamedFileAsync)(IStorageFileStatics *, HSTRING, void *, void *,
                                              IAsyncOperation_StorageFile **);
    HRESULT (WINAPI *ReplaceWithStreamedFileAsync)(IStorageFileStatics *, IStorageFile *,
                                                   void *, void *,
                                                   IAsyncOperation_StorageFile **);
    HRESULT (WINAPI *CreateStreamedFileFromUriAsync)(IStorageFileStatics *, HSTRING, void *,
                                                     void *, IAsyncOperation_StorageFile **);
    HRESULT (WINAPI *ReplaceWithStreamedFileFromUriAsync)(IStorageFileStatics *,
                                                          IStorageFile *, void *, void *,
                                                          IAsyncOperation_StorageFile **);
} IStorageFileStaticsVtbl;

struct IStorageFileStatics
{
    const IStorageFileStaticsVtbl *lpVtbl;
};

struct storage_file_factory
{
    IActivationFactory IActivationFactory_iface;
    IStorageFileStatics IStorageFileStatics_iface;
    LONG ref;
};

struct storage_file
{
    IStorageFile IStorageFile_iface;
    IStorageItem IStorageItem_iface;
    LONG ref;
    HSTRING path;
};

struct storage_file_operation
{
    IAsyncOperation_StorageFile IAsyncOperation_StorageFile_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    LONG handler_set;
    LONG closed;
    HRESULT error;
    IAsyncOperationCompletedHandler_StorageFile *handler;
    IStorageFile *result;
};

static inline struct storage_file_factory *factory_from_activation(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct storage_file_factory, IActivationFactory_iface);
}

static inline struct storage_file_factory *factory_from_statics(IStorageFileStatics *iface)
{
    return CONTAINING_RECORD(iface, struct storage_file_factory, IStorageFileStatics_iface);
}

static inline struct storage_file *file_from_file(IStorageFile *iface)
{
    return CONTAINING_RECORD(iface, struct storage_file, IStorageFile_iface);
}

static inline struct storage_file *file_from_item(IStorageItem *iface)
{
    return CONTAINING_RECORD(iface, struct storage_file, IStorageItem_iface);
}

static inline struct storage_file_operation *operation_from_async(IAsyncOperation_StorageFile *iface)
{
    return CONTAINING_RECORD(iface, struct storage_file_operation,
                             IAsyncOperation_StorageFile_iface);
}

static inline struct storage_file_operation *operation_from_info(IAsyncInfo *iface)
{
    return CONTAINING_RECORD(iface, struct storage_file_operation, IAsyncInfo_iface);
}

static HRESULT copy_iids(ULONG *count, IID **iids, UINT size, const IID *const *values)
{
    UINT i;

    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = NULL;
    if (!(*iids = CoTaskMemAlloc(size * sizeof(**iids)))) return E_OUTOFMEMORY;
    for (i = 0; i < size; ++i) (*iids)[i] = *values[i];
    *count = size;
    return S_OK;
}

static HRESULT runtime_class_name(HSTRING *name)
{
    if (!name) return E_POINTER;
    return WindowsCreateString(RuntimeClass_Windows_Storage_StorageFile,
                               ARRAY_SIZE(RuntimeClass_Windows_Storage_StorageFile) - 1, name);
}

static HRESULT trust_level(TrustLevel *level)
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT file_query_interface(struct storage_file *file, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) || IsEqualGUID(iid, &IID_IStorageFile))
        *out = &file->IStorageFile_iface;
    else if (IsEqualGUID(iid, &IID_IStorageItem))
        *out = &file->IStorageItem_iface;
    else
        return E_NOINTERFACE;
    InterlockedIncrement(&file->ref);
    return S_OK;
}

static ULONG file_add_ref(struct storage_file *file)
{
    return InterlockedIncrement(&file->ref);
}

static ULONG file_release(struct storage_file *file)
{
    ULONG ref = InterlockedDecrement(&file->ref);
    if (!ref)
    {
        WindowsDeleteString(file->path);
        free(file);
    }
    return ref;
}

static HRESULT WINAPI file_QueryInterface(IStorageFile *iface, REFIID iid, void **out)
{
    return file_query_interface(file_from_file(iface), iid, out);
}

static ULONG WINAPI file_AddRef(IStorageFile *iface)
{
    return file_add_ref(file_from_file(iface));
}

static ULONG WINAPI file_Release(IStorageFile *iface)
{
    return file_release(file_from_file(iface));
}

static HRESULT WINAPI file_GetIids(IStorageFile *iface, ULONG *count, IID **iids)
{
    static const IID *values[] = {&IID_IStorageFile, &IID_IStorageItem};
    return copy_iids(count, iids, ARRAY_SIZE(values), values);
}

static HRESULT WINAPI file_GetRuntimeClassName(IStorageFile *iface, HSTRING *name)
{
    return runtime_class_name(name);
}

static HRESULT WINAPI file_GetTrustLevel(IStorageFile *iface, TrustLevel *level)
{
    return trust_level(level);
}

static HRESULT WINAPI file_get_FileType(IStorageFile *iface, HSTRING *value)
{
    const WCHAR *path, *name, *extension;
    UINT32 length;

    if (!value) return E_POINTER;
    path = WindowsGetStringRawBuffer(file_from_file(iface)->path, &length);
    name = wcsrchr(path, '\\');
    name = name ? name + 1 : path;
    extension = wcsrchr(name, '.');
    if (!extension) extension = path + length;
    return WindowsCreateString(extension, path + length - extension, value);
}

static HRESULT WINAPI file_get_ContentType(IStorageFile *iface, HSTRING *value)
{
    if (!value) return E_POINTER;
    return WindowsCreateString(NULL, 0, value);
}

static HRESULT WINAPI file_OpenAsync(IStorageFile *iface, FileAccessMode mode,
                                     IAsyncOperation_IRandomAccessStream **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_OpenTransactedWriteAsync(
    IStorageFile *iface, IAsyncOperation_StorageStreamTransaction **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_CopyDefault(IStorageFile *iface, IStorageFolder *folder,
                                       IAsyncOperation_StorageFile **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_CopyName(IStorageFile *iface, IStorageFolder *folder, HSTRING name,
                                    IAsyncOperation_StorageFile **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_Copy(IStorageFile *iface, IStorageFolder *folder, HSTRING name,
                                NameCollisionOption option,
                                IAsyncOperation_StorageFile **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_CopyAndReplace(IStorageFile *iface, IStorageFile *file,
                                          IAsyncAction **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_MoveDefault(IStorageFile *iface, IStorageFolder *folder,
                                       IAsyncAction **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_MoveName(IStorageFile *iface, IStorageFolder *folder, HSTRING name,
                                    IAsyncAction **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_Move(IStorageFile *iface, IStorageFolder *folder, HSTRING name,
                                NameCollisionOption option, IAsyncAction **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI file_MoveAndReplace(IStorageFile *iface, IStorageFile *file,
                                          IAsyncAction **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static const IStorageFileVtbl file_vtbl =
{
    file_QueryInterface,
    file_AddRef,
    file_Release,
    file_GetIids,
    file_GetRuntimeClassName,
    file_GetTrustLevel,
    file_get_FileType,
    file_get_ContentType,
    file_OpenAsync,
    file_OpenTransactedWriteAsync,
    file_CopyDefault,
    file_CopyName,
    file_Copy,
    file_CopyAndReplace,
    file_MoveDefault,
    file_MoveName,
    file_Move,
    file_MoveAndReplace,
};

static HRESULT WINAPI item_QueryInterface(IStorageItem *iface, REFIID iid, void **out)
{
    return file_query_interface(file_from_item(iface), iid, out);
}

static ULONG WINAPI item_AddRef(IStorageItem *iface)
{
    return file_add_ref(file_from_item(iface));
}

static ULONG WINAPI item_Release(IStorageItem *iface)
{
    return file_release(file_from_item(iface));
}

static HRESULT WINAPI item_GetIids(IStorageItem *iface, ULONG *count, IID **iids)
{
    return file_GetIids(&file_from_item(iface)->IStorageFile_iface, count, iids);
}

static HRESULT WINAPI item_GetRuntimeClassName(IStorageItem *iface, HSTRING *name)
{
    return runtime_class_name(name);
}

static HRESULT WINAPI item_GetTrustLevel(IStorageItem *iface, TrustLevel *level)
{
    return trust_level(level);
}

static HRESULT WINAPI item_RenameDefault(IStorageItem *iface, HSTRING name,
                                         IAsyncAction **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI item_Rename(IStorageItem *iface, HSTRING name, NameCollisionOption option,
                                  IAsyncAction **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI item_DeleteDefault(IStorageItem *iface, IAsyncAction **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI item_Delete(IStorageItem *iface, StorageDeleteOption option,
                                  IAsyncAction **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI item_GetBasicProperties(
    IStorageItem *iface, IAsyncOperation_BasicProperties **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI item_get_Name(IStorageItem *iface, HSTRING *value)
{
    struct storage_file *file = file_from_item(iface);
    const WCHAR *path, *name;
    UINT32 length;

    if (!value) return E_POINTER;
    path = WindowsGetStringRawBuffer(file->path, &length);
    name = wcsrchr(path, '\\');
    name = name ? name + 1 : path;
    return WindowsCreateString(name, path + length - name, value);
}

static HRESULT WINAPI item_get_Path(IStorageItem *iface, HSTRING *value)
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString(file_from_item(iface)->path, value);
}

static HRESULT WINAPI item_get_Attributes(IStorageItem *iface, FileAttributes *value)
{
    DWORD attributes;

    if (!value) return E_POINTER;
    attributes = GetFileAttributesW(WindowsGetStringRawBuffer(file_from_item(iface)->path, NULL));
    if (attributes == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(GetLastError());
    *value = FileAttributes_Normal;
    if (attributes & FILE_ATTRIBUTE_READONLY) *value |= FileAttributes_ReadOnly;
    if (attributes & FILE_ATTRIBUTE_ARCHIVE) *value |= FileAttributes_Archive;
    if (attributes & FILE_ATTRIBUTE_TEMPORARY) *value |= FileAttributes_Temporary;
    return S_OK;
}

static HRESULT WINAPI item_get_DateCreated(IStorageItem *iface, DateTime *value)
{
    WIN32_FILE_ATTRIBUTE_DATA data;

    if (!value) return E_POINTER;
    if (!GetFileAttributesExW(WindowsGetStringRawBuffer(file_from_item(iface)->path, NULL),
                              GetFileExInfoStandard, &data))
        return HRESULT_FROM_WIN32(GetLastError());
    value->UniversalTime = ((INT64)data.ftCreationTime.dwHighDateTime << 32) |
                           data.ftCreationTime.dwLowDateTime;
    return S_OK;
}

static HRESULT WINAPI item_IsOfType(IStorageItem *iface, StorageItemTypes type, boolean *value)
{
    if (!value) return E_POINTER;
    *value = type == StorageItemTypes_File;
    return S_OK;
}

static const IStorageItemVtbl item_vtbl =
{
    item_QueryInterface,
    item_AddRef,
    item_Release,
    item_GetIids,
    item_GetRuntimeClassName,
    item_GetTrustLevel,
    item_RenameDefault,
    item_Rename,
    item_DeleteDefault,
    item_Delete,
    item_GetBasicProperties,
    item_get_Name,
    item_get_Path,
    item_get_Attributes,
    item_get_DateCreated,
    item_IsOfType,
};

static HRESULT storage_file_create(HSTRING path, IStorageFile **out)
{
    struct storage_file *file;
    const WCHAR *buffer;
    UINT32 length;
    DWORD attributes;
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = NULL;
    buffer = WindowsGetStringRawBuffer(path, &length);
    if (!length) return E_INVALIDARG;
    attributes = GetFileAttributesW(buffer);
    if (attributes == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(GetLastError());
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return E_INVALIDARG;
    if (!(file = calloc(1, sizeof(*file)))) return E_OUTOFMEMORY;
    file->IStorageFile_iface.lpVtbl = &file_vtbl;
    file->IStorageItem_iface.lpVtbl = &item_vtbl;
    file->ref = 1;
    if (FAILED(hr = WindowsDuplicateString(path, &file->path)))
    {
        free(file);
        return hr;
    }
    *out = &file->IStorageFile_iface;
    return S_OK;
}

static HRESULT operation_query_interface(struct storage_file_operation *operation,
                                         REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) ||
        IsEqualGUID(iid, &IID_IAsyncOperation_StorageFile))
        *out = &operation->IAsyncOperation_StorageFile_iface;
    else if (IsEqualGUID(iid, &IID_IAsyncInfo))
        *out = &operation->IAsyncInfo_iface;
    else
        return E_NOINTERFACE;
    InterlockedIncrement(&operation->ref);
    return S_OK;
}

static ULONG operation_add_ref(struct storage_file_operation *operation)
{
    return InterlockedIncrement(&operation->ref);
}

static ULONG operation_release(struct storage_file_operation *operation)
{
    ULONG ref = InterlockedDecrement(&operation->ref);
    if (!ref)
    {
        if (operation->handler)
            IAsyncOperationCompletedHandler_StorageFile_Release(operation->handler);
        if (operation->result) IStorageFile_Release(operation->result);
        free(operation);
    }
    return ref;
}

static HRESULT WINAPI operation_QueryInterface(IAsyncOperation_StorageFile *iface,
                                               REFIID iid, void **out)
{
    return operation_query_interface(operation_from_async(iface), iid, out);
}

static ULONG WINAPI operation_AddRef(IAsyncOperation_StorageFile *iface)
{
    return operation_add_ref(operation_from_async(iface));
}

static ULONG WINAPI operation_Release(IAsyncOperation_StorageFile *iface)
{
    return operation_release(operation_from_async(iface));
}

static HRESULT WINAPI operation_GetIids(IAsyncOperation_StorageFile *iface,
                                        ULONG *count, IID **iids)
{
    static const IID *values[] = {&IID_IAsyncOperation_StorageFile, &IID_IAsyncInfo};
    return copy_iids(count, iids, ARRAY_SIZE(values), values);
}

static HRESULT WINAPI operation_GetRuntimeClassName(IAsyncOperation_StorageFile *iface,
                                                    HSTRING *name)
{
    if (!name) return E_POINTER;
    *name = NULL;
    return S_OK;
}

static HRESULT WINAPI operation_GetTrustLevel(IAsyncOperation_StorageFile *iface,
                                              TrustLevel *level)
{
    return trust_level(level);
}

static HRESULT WINAPI operation_put_Completed(
    IAsyncOperation_StorageFile *iface, IAsyncOperationCompletedHandler_StorageFile *handler)
{
    struct storage_file_operation *operation = operation_from_async(iface);
    AsyncStatus status;

    if (InterlockedCompareExchange(&operation->closed, 0, 0)) return E_ILLEGAL_METHOD_CALL;
    if (InterlockedCompareExchange(&operation->handler_set, 1, 0))
        return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    if (!handler) return S_OK;
    IAsyncOperationCompletedHandler_StorageFile_AddRef(handler);
    operation->handler = handler;
    status = FAILED(operation->error) ? Error : Completed;
    operation_add_ref(operation);
    IAsyncOperationCompletedHandler_StorageFile_Invoke(handler, iface, status);
    operation_release(operation);
    return S_OK;
}

static HRESULT WINAPI operation_get_Completed(
    IAsyncOperation_StorageFile *iface, IAsyncOperationCompletedHandler_StorageFile **handler)
{
    struct storage_file_operation *operation = operation_from_async(iface);

    if (!handler) return E_POINTER;
    *handler = NULL;
    if (InterlockedCompareExchange(&operation->closed, 0, 0)) return E_ILLEGAL_METHOD_CALL;
    if (operation->handler)
        IAsyncOperationCompletedHandler_StorageFile_AddRef(*handler = operation->handler);
    return S_OK;
}

static HRESULT WINAPI operation_GetResults(IAsyncOperation_StorageFile *iface,
                                           IStorageFile **result)
{
    struct storage_file_operation *operation = operation_from_async(iface);

    if (!result) return E_POINTER;
    *result = NULL;
    if (InterlockedCompareExchange(&operation->closed, 0, 0)) return E_ILLEGAL_METHOD_CALL;
    if (FAILED(operation->error)) return operation->error;
    IStorageFile_AddRef(*result = operation->result);
    return S_OK;
}

static const IAsyncOperation_StorageFileVtbl operation_vtbl =
{
    operation_QueryInterface,
    operation_AddRef,
    operation_Release,
    operation_GetIids,
    operation_GetRuntimeClassName,
    operation_GetTrustLevel,
    operation_put_Completed,
    operation_get_Completed,
    operation_GetResults,
};

static HRESULT WINAPI info_QueryInterface(IAsyncInfo *iface, REFIID iid, void **out)
{
    return operation_query_interface(operation_from_info(iface), iid, out);
}

static ULONG WINAPI info_AddRef(IAsyncInfo *iface)
{
    return operation_add_ref(operation_from_info(iface));
}

static ULONG WINAPI info_Release(IAsyncInfo *iface)
{
    return operation_release(operation_from_info(iface));
}

static HRESULT WINAPI info_GetIids(IAsyncInfo *iface, ULONG *count, IID **iids)
{
    return operation_GetIids(&operation_from_info(iface)->IAsyncOperation_StorageFile_iface,
                             count, iids);
}

static HRESULT WINAPI info_GetRuntimeClassName(IAsyncInfo *iface, HSTRING *name)
{
    if (!name) return E_POINTER;
    *name = NULL;
    return S_OK;
}

static HRESULT WINAPI info_GetTrustLevel(IAsyncInfo *iface, TrustLevel *level)
{
    return trust_level(level);
}

static HRESULT WINAPI info_get_Id(IAsyncInfo *iface, UINT32 *id)
{
    if (!id) return E_POINTER;
    if (InterlockedCompareExchange(&operation_from_info(iface)->closed, 0, 0))
        return E_ILLEGAL_METHOD_CALL;
    *id = 1;
    return S_OK;
}

static HRESULT WINAPI info_get_Status(IAsyncInfo *iface, AsyncStatus *status)
{
    struct storage_file_operation *operation = operation_from_info(iface);

    if (!status) return E_POINTER;
    if (InterlockedCompareExchange(&operation->closed, 0, 0)) return E_ILLEGAL_METHOD_CALL;
    *status = FAILED(operation->error) ? Error : Completed;
    return S_OK;
}

static HRESULT WINAPI info_get_ErrorCode(IAsyncInfo *iface, HRESULT *error)
{
    struct storage_file_operation *operation = operation_from_info(iface);

    if (!error) return E_POINTER;
    if (InterlockedCompareExchange(&operation->closed, 0, 0)) return E_ILLEGAL_METHOD_CALL;
    *error = operation->error;
    return S_OK;
}

static HRESULT WINAPI info_Cancel(IAsyncInfo *iface)
{
    return InterlockedCompareExchange(&operation_from_info(iface)->closed, 0, 0)
           ? E_ILLEGAL_METHOD_CALL : S_OK;
}

static HRESULT WINAPI info_Close(IAsyncInfo *iface)
{
    return InterlockedExchange(&operation_from_info(iface)->closed, 1)
           ? E_ILLEGAL_METHOD_CALL : S_OK;
}

static const IAsyncInfoVtbl info_vtbl =
{
    info_QueryInterface,
    info_AddRef,
    info_Release,
    info_GetIids,
    info_GetRuntimeClassName,
    info_GetTrustLevel,
    info_get_Id,
    info_get_Status,
    info_get_ErrorCode,
    info_Cancel,
    info_Close,
};

static HRESULT storage_file_operation_create(HSTRING path, IAsyncOperation_StorageFile **out)
{
    struct storage_file_operation *operation;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!(operation = calloc(1, sizeof(*operation)))) return E_OUTOFMEMORY;
    operation->IAsyncOperation_StorageFile_iface.lpVtbl = &operation_vtbl;
    operation->IAsyncInfo_iface.lpVtbl = &info_vtbl;
    operation->ref = 1;
    operation->error = storage_file_create(path, &operation->result);
    *out = &operation->IAsyncOperation_StorageFile_iface;
    return S_OK;
}

static HRESULT factory_query_interface(struct storage_file_factory *factory,
                                       REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) || IsEqualGUID(iid, &IID_IActivationFactory))
        *out = &factory->IActivationFactory_iface;
    else if (IsEqualGUID(iid, &storage_file_statics_iid))
        *out = &factory->IStorageFileStatics_iface;
    else
        return E_NOINTERFACE;
    InterlockedIncrement(&factory->ref);
    return S_OK;
}

static HRESULT WINAPI factory_QueryInterface(IActivationFactory *iface, REFIID iid, void **out)
{
    return factory_query_interface(factory_from_activation(iface), iid, out);
}

static ULONG WINAPI factory_AddRef(IActivationFactory *iface)
{
    return InterlockedIncrement(&factory_from_activation(iface)->ref);
}

static ULONG WINAPI factory_Release(IActivationFactory *iface)
{
    return InterlockedDecrement(&factory_from_activation(iface)->ref);
}

static HRESULT WINAPI factory_GetIids(IActivationFactory *iface, ULONG *count, IID **iids)
{
    static const IID *values[] = {&storage_file_statics_iid};
    return copy_iids(count, iids, ARRAY_SIZE(values), values);
}

static HRESULT WINAPI factory_GetRuntimeClassName(IActivationFactory *iface, HSTRING *name)
{
    return runtime_class_name(name);
}

static HRESULT WINAPI factory_GetTrustLevel(IActivationFactory *iface, TrustLevel *level)
{
    return trust_level(level);
}

static HRESULT WINAPI factory_ActivateInstance(IActivationFactory *iface, IInspectable **instance)
{
    if (instance) *instance = NULL;
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

static HRESULT WINAPI statics_QueryInterface(IStorageFileStatics *iface, REFIID iid, void **out)
{
    return factory_query_interface(factory_from_statics(iface), iid, out);
}

static ULONG WINAPI statics_AddRef(IStorageFileStatics *iface)
{
    return InterlockedIncrement(&factory_from_statics(iface)->ref);
}

static ULONG WINAPI statics_Release(IStorageFileStatics *iface)
{
    return InterlockedDecrement(&factory_from_statics(iface)->ref);
}

static HRESULT WINAPI statics_GetIids(IStorageFileStatics *iface, ULONG *count, IID **iids)
{
    static const IID *values[] = {&storage_file_statics_iid};
    return copy_iids(count, iids, ARRAY_SIZE(values), values);
}

static HRESULT WINAPI statics_GetRuntimeClassName(IStorageFileStatics *iface, HSTRING *name)
{
    return runtime_class_name(name);
}

static HRESULT WINAPI statics_GetTrustLevel(IStorageFileStatics *iface, TrustLevel *level)
{
    return trust_level(level);
}

static HRESULT WINAPI statics_GetFileFromPathAsync(IStorageFileStatics *iface, HSTRING path,
                                                   IAsyncOperation_StorageFile **operation)
{
    return storage_file_operation_create(path, operation);
}

static HRESULT WINAPI statics_not_implemented(IStorageFileStatics *iface, void *arg,
                                              IAsyncOperation_StorageFile **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_not_implemented4(IStorageFileStatics *iface, void *arg1,
                                               void *arg2, void *arg3,
                                               IAsyncOperation_StorageFile **operation)
{
    if (operation) *operation = NULL;
    return E_NOTIMPL;
}

static const IStorageFileStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    statics_GetFileFromPathAsync,
    statics_not_implemented,
    (void *)statics_not_implemented4,
    (void *)statics_not_implemented4,
    (void *)statics_not_implemented4,
    (void *)statics_not_implemented4,
};

static struct storage_file_factory storage_file_factory_impl =
{
    {&factory_vtbl},
    {&statics_vtbl},
    1,
};

IActivationFactory *storage_file_factory = &storage_file_factory_impl.IActivationFactory_iface;
