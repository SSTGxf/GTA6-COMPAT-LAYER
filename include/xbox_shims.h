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

// ---- XGameSave update flow (phase 3) ----
// Signatures grounded from Microsoft Learn (XGameSave.h, xgameruntime.lib).
// An update is a staged, atomic set of blob writes/deletes for one
// container; XGameSaveSubmitUpdate applies it to the container folder.
// GS_MAX_BLOB_SIZE is 16 MB per blob and per update (GDK docs).
typedef void* XGameSaveUpdateHandle;
#define GS_MAX_BLOB_SIZE (16u * 1024u * 1024u)
HRESULT XGameSaveCreateUpdate(XGameSaveContainerHandle container,
                              const char* containerDisplayName,
                              XGameSaveUpdateHandle* update);
HRESULT XGameSaveSubmitBlobWrite(XGameSaveUpdateHandle update,
                                 const char* blobName,
                                 const uint8_t* data, size_t byteCount);
HRESULT XGameSaveSubmitBlobDelete(XGameSaveUpdateHandle update,
                                  const char* blobName);
HRESULT XGameSaveSubmitUpdate(XGameSaveUpdateHandle update);
HRESULT XGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle update,
                                   struct XAsyncBlock* async);
HRESULT XGameSaveSubmitUpdateResult(struct XAsyncBlock* async);
HRESULT XGameSaveCloseUpdate(XGameSaveUpdateHandle update);
HRESULT XGameSaveCloseContainer(XGameSaveContainerHandle container);
HRESULT XGameSaveReadBlobData(XGameSaveContainerHandle container,
                              const char* blobName, size_t* bytesRead,
                              size_t bufferSize, uint8_t* buffer);

// ---- XGameSaveFiles (phase 3) ----
// File-style saves (the GDK-recommended API): GetFolderWithUiAsync
// returns a folder path and the title uses plain Win32 FileIO under it.
// Headless: no UI is shown; the local provider folder is returned.
// NOTE: the real GDK errors with E_GS_PROVIDER_MISMATCH if a title mixes
// XGameSave and XGameSaveFiles; this layer's local-only model keeps their
// roots separate, so both work locally (deliberate deviation, flagged).
HRESULT XGameSaveFilesGetFolderWithUiAsync(XUserHandle requestingUser,
                                           const char* configurationId,
                                           struct XAsyncBlock* async);
HRESULT XGameSaveFilesGetFolderWithUiResult(struct XAsyncBlock* async,
                                            size_t bufferSize, char* folder);

// ---- XPackage (phase 2, milestone E; completed phase 3) ----
// The mount path is the directory holding the guest PE (set by the
// loader). The layer presents the guest as a packaged Game Core title.
// Package identifier is synthetic (local-only, no store install).
#define XPACKAGE_IDENTIFIER_MAX_LENGTH 64
typedef void* XPackageMountHandle;
// XVersion / XPackageKind / XPackageEnumerationScope / XPackageDetails
// layouts are from the public GDK docs (XPackage.h); enum values are
// the public ones (Game=0, Content=1; ThisOnly=0, ThisAndRelated=1,
// ThisPublisher=2).
typedef struct XVersion {
    uint32_t major;
    uint32_t minor;
    uint32_t build;
    uint32_t revision;
} XVersion;
typedef enum XPackageKind {
    XPackageKind_Game = 0,
    XPackageKind_Content = 1,
} XPackageKind;
typedef enum XPackageEnumerationScope {
    XPackageEnumerationScope_ThisOnly = 0,
    XPackageEnumerationScope_ThisAndRelated = 1,
    XPackageEnumerationScope_ThisPublisher = 2,
} XPackageEnumerationScope;
typedef struct XPackageDetails {
    const char* packageIdentifier;
    XVersion version;
    XPackageKind kind;
    const char* displayName;
    const char* description;
    const char* publisher;
    const char* storeId;
    bool installing;
    uint32_t index;
    uint32_t count;
    bool ageRestricted;
    const char* titleID;
} XPackageDetails;
typedef bool (CALLBACK XPackageEnumerationCallback)(
    void* context, const XPackageDetails* details);
HRESULT XPackageGetMountPathSize(uint32_t* size);
HRESULT XPackageGetMountPath(wchar_t* path, uint32_t* pathSize);
BOOL XPackageIsPackagedProcess(void);
// Real signature (Microsoft Learn): (bufferSize, char* buffer). Phase 2
// had this wrong as (void*) — fixed in phase 3.
HRESULT XPackageGetCurrentProcessPackageIdentifier(size_t bufferSize,
                                                   char* buffer);
HRESULT XPackageEnumeratePackages(XPackageKind kind,
                                  XPackageEnumerationScope scope,
                                  void* context,
                                  XPackageEnumerationCallback* callback);
HRESULT XPackageMountWithUiAsync(const char* packageIdentifier,
                                 struct XAsyncBlock* async);
HRESULT XPackageMountWithUiResult(struct XAsyncBlock* async,
                                  XPackageMountHandle* mount);
HRESULT XPackageCloseMountHandle(XPackageMountHandle mount);

#ifdef __cplusplus
}
#endif
