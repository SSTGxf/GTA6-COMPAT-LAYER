// hle_registry.h — the HLE export registry (Xenia-style).
// Central table mapping (Xbox DLL, function name) -> host implementation +
// status tag. The loader resolves guest imports through this table.
#pragma once
#include "scarlett.h"
#include "xbox_shims.h"  // XAsyncBlock / XUser / XGameSave / XPackage types
#include <d3d12.h>  // IUnknown + D3D_FEATURE_LEVEL for the device-creation shim

namespace sc {

// Host implementations of the shimmed Xbox functions.
extern "C" HRESULT HleXGameRuntimeInitialize();
extern "C" void* HleXMemAlloc(SIZE_T size, DWORD attrs);
extern "C" void HleXMemFree(void* p);
extern "C" HRESULT HleD3D12XboxCreateDevice(IUnknown* adapter,
                                            D3D_FEATURE_LEVEL minFL,
                                            REFIID riid, void** ppDevice);
extern "C" HRESULT HleXAudio2Create(void** ppXAudio2, UINT32 flags, DWORD proc);
extern "C" HRESULT HleGameInputCreate(void** ppGameInput);
extern "C" HRESULT HleDStorageGetFactory(void** ppFactory);
extern "C" HRESULT HleSetFrameIntervalX(UINT32 interval, UINT32 flags);
extern "C" HRESULT HleScheduleFrameEventX(UINT64 frameId);
extern "C" HRESULT HleWaitFrameEventX(UINT64 frameId, DWORD timeoutMs);

// XAsync / XUser / XGameSave / XPackage (phase 2, milestone E).
// Types come from xbox_shims.h (shared with the guest).
extern "C" HRESULT HleXAsyncGetStatus(struct XAsyncBlock* async, BOOL wait);
extern "C" HRESULT HleXAsyncGetResultSize(struct XAsyncBlock* async,
                                           size_t* size);
extern "C" HRESULT HleXAsyncCancel(struct XAsyncBlock* async);
extern "C" HRESULT HleXAsyncRun(struct XAsyncBlock* async, XAsyncWork work);
extern "C" HRESULT HleXUserAddAsync(XUserAddOptions options,
                                     struct XAsyncBlock* async);
extern "C" HRESULT HleXUserAddResult(struct XAsyncBlock* async,
                                     XUserHandle* user);
extern "C" HRESULT HleXUserGetId(XUserHandle user, uint64_t* xuid);
extern "C" HRESULT HleXUserGetGamertag(XUserHandle user,
                                        XUserGamertagOptions options,
                                        char* gamertag, size_t* gamertagSize);
extern "C" HRESULT HleXUserGetLocalId(XUserHandle user, XUserLocalId* localId);
extern "C" HRESULT HleXUserCloseHandle(XUserHandle user);
extern "C" HRESULT HleXGameSaveInitializeProvider(XUserHandle user,
                                                   const char* scid,
                                                   BOOL allowSynchronous,
                                                   XGameSaveProviderHandle* provider);
extern "C" HRESULT HleXGameSaveCloseProvider(XGameSaveProviderHandle provider);
extern "C" HRESULT HleXGameSaveCreateContainer(XGameSaveProviderHandle provider,
                                                const char* displayName,
                                                XGameSaveContainerHandle* container);
extern "C" HRESULT HleXPackageGetMountPathSize(uint32_t* size);
extern "C" HRESULT HleXPackageGetMountPath(wchar_t* path, uint32_t* pathSize);
extern "C" BOOL HleXPackageIsPackagedProcess(void);
extern "C" HRESULT HleXPackageGetCurrentProcessPackageIdentifier(void* identifier);
extern "C" HRESULT HleXPackageMountWithUiAsync(struct XAsyncBlock* async);
extern "C" HRESULT HleXPackageMountWithUiResult(struct XAsyncBlock* async,
                                                 void** mountHandle);
extern "C" HRESULT HleXPackageCloseMountHandle(void* mountHandle);

// Guest directory for XPackageGetMountPath (set by the loader).
void SetGuestDir(const wchar_t* dir);

}  // namespace sc
