// xbox_shims.h — guest-side prototypes for the Xbox (Scarlett) functions the
// layer shims. These are the signatures the SYNTHETIC guest title calls.
// Signatures marked ASSUMED are D3D12CreateDevice-shaped guesses for
// Xbox-only entry points whose real signatures are GDKX/NDA-gated; they MUST
// be confirmed against the real exe's import table + GDKX headers once the
// exe lands. Do not treat them as ground truth.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <d3d12.h>  // IUnknown + D3D_FEATURE_LEVEL (D3D12XboxCreateDevice)

#ifdef __cplusplus
extern "C" {
#endif

// XGameRuntimeInitialize — public GDK API (XGameRuntime.h). First call a
// GDK title makes. Links XGameRuntime.lib; runtime is xgameruntime.dll.
HRESULT XGameRuntimeInitialize(void);

// XMemAlloc / XMemFree — Xbox extended memory API (xmem.dll).
// XMemAlloc(size, attributes). Attributes enum is GDKX-gated; the stub
// ignores it and allocates from the process heap.
void* XMemAlloc(SIZE_T size, DWORD allocationAttributes);
void XMemFree(void* p);

// D3D12XboxCreateDevice — D3D12CreateDevice-shaped signature (ASSUMED).
// Real Xbox form takes D3D12XBOX_CREATE_DEVICE_PARAMETERS (GDKX-gated);
// the layer translates the assumed form to a real desktop D3D12 device
// (phase 2). Confirm the real form against the exe's import table once
// it lands.
HRESULT D3D12XboxCreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL minFeatureLevel,
                              REFIID riid, void** ppDevice);

// XAudio2Create — public XAudio2 2.9 signature (xaudio2.h).
struct IXAudio2;
HRESULT XAudio2Create(IXAudio2** ppXAudio2, UINT32 flags, DWORD processor);

// GameInputCreate — public GameInput API.
struct IGameInput;
HRESULT GameInputCreate(IGameInput** ppGameInput);

// DirectStorage factory — ASSUMED. Xbox variant is dstorage_xs.
HRESULT DStorageGetFactory(void** ppFactory);

// Frame pacing — real (phase 2): SetFrameIntervalX stores the present
// interval (1 = 60 Hz), ScheduleFrameEventX stamps a frame, and
// WaitFrameEventX sleeps until that frame's deadline (bounded by the
// timeout). Signatures are ASSUMED; exact parameter shapes are TBD
// from the exe.
HRESULT SetFrameIntervalX(UINT32 interval, UINT32 flags);
HRESULT ScheduleFrameEventX(UINT64 frameId);
HRESULT WaitFrameEventX(UINT64 frameId, DWORD timeoutMs);

// ---- XAsync (phase 2, milestone E) ----
// Async model: the layer completes operations SYNCHRONOUSLY — the
// provider does the work inline, stores the result in the block, and
// invokes the callback before returning. So by the time an *Async call
// returns, XAsyncGetStatus reports the final HRESULT. Signatures are
// grounded from Microsoft Learn (XAsyncBlock = {queue, context,
// callback, internal[4 pointers]}); there is no public XAsyncComplete /
// XAsyncGetResult — results come from per-call ...Result APIs.
struct XAsyncBlock;
typedef void (CALLBACK *XAsyncCompletionRoutine)(struct XAsyncBlock* async);
typedef void (CALLBACK *XAsyncWork)(struct XAsyncBlock* async);
struct XAsyncBlock {
    void* queue;     // XTaskQueueHandle, opaque (may be NULL)
    void* context;   // caller context
    XAsyncCompletionRoutine callback;
    // Provider use: [0] = HRESULT status (E_PENDING until complete),
    // [1] = result size, [2] = result pointer, [3] = completed flag.
    void* internal[4];
};
HRESULT XAsyncGetStatus(struct XAsyncBlock* async, BOOL wait);
HRESULT XAsyncGetResultSize(struct XAsyncBlock* async, size_t* size);
HRESULT XAsyncCancel(struct XAsyncBlock* async);
HRESULT XAsyncRun(struct XAsyncBlock* async, XAsyncWork work);

// ---- XUser (phase 2, milestone E) ----
// Simplified user model: only the silent default user works; interactive
// sign-in is rejected with E_INVALIDARG (per the GDK docs). The default
// user is a synthetic local user ("Player") — no real Xbox Live sign-in.
typedef void* XUserHandle;
typedef struct XUserLocalId { uint64_t value; } XUserLocalId;
typedef enum XUserAddOptions {
    XUserAddOptions_None = 0,
    XUserAddOptions_AddDefaultUserSilently = 1,
    XUserAddOptions_AddDefaultUserAllowingUI = 2,
    XUserAddOptions_AllowGuests = 4,
} XUserAddOptions;
typedef enum XUserGamertagOptions {
    XUserGamertagOptions_UniqueModern = 0,
    XUserGamertagOptions_Classic = 1,
} XUserGamertagOptions;
HRESULT XUserAddAsync(XUserAddOptions options, struct XAsyncBlock* async);
HRESULT XUserAddResult(struct XAsyncBlock* async, XUserHandle* user);
HRESULT XUserGetId(XUserHandle user, uint64_t* xuid);
HRESULT XUserGetGamertag(XUserHandle user, XUserGamertagOptions options,
                         char* gamertag, size_t* gamertagSize);
HRESULT XUserGetLocalId(XUserHandle user, XUserLocalId* localId);
HRESULT XUserCloseHandle(XUserHandle user);

// ---- XGameSave (phase 2, milestone E) ----
// Provider rooted at a local folder:
// %LOCALAPPDATA%\ScarlettCompat\XGameSave\<SCID>\. Containers are
// subfolders of the provider root. Real cloud sync is NOT implemented
// (local only, like a machine-only provider).
typedef void* XGameSaveProviderHandle;
typedef void* XGameSaveContainerHandle;
HRESULT XGameSaveInitializeProvider(XUserHandle user, const char* scid,
                                    BOOL allowSynchronous,
                                    XGameSaveProviderHandle* provider);
HRESULT XGameSaveCloseProvider(XGameSaveProviderHandle provider);
HRESULT XGameSaveCreateContainer(XGameSaveProviderHandle provider,
                                 const char* displayName,
                                 XGameSaveContainerHandle* container);

// ---- XPackage (phase 2, milestone E) ----
// The mount path is the directory holding the guest PE (set by the
// loader). The layer presents the guest as a packaged Game Core title.
HRESULT XPackageGetMountPathSize(uint32_t* size);
HRESULT XPackageGetMountPath(wchar_t* path, uint32_t* pathSize);
BOOL XPackageIsPackagedProcess(void);
HRESULT XPackageGetCurrentProcessPackageIdentifier(void* identifier);
HRESULT XPackageMountWithUiAsync(struct XAsyncBlock* async);
HRESULT XPackageMountWithUiResult(struct XAsyncBlock* async,
                                  void** mountHandle);
HRESULT XPackageCloseMountHandle(void* mountHandle);

#ifdef __cplusplus
}
#endif
