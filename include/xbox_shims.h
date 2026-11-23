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

// D3D12XboxCreateDevice — REAL signature (grounded 2026-11-23 from the
// real exe's call site at 0x1424785b7, corroborated by the public Xbox
// SDK guide's sample: D3D12XboxCreateDevice(nullptr, &params,
// IID_PPV_ARGS(&device))).
//   HRESULT D3D12XboxCreateDevice(IUnknown* pAdapter,
//       const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters,
//       REFIID riid, void** ppDevice);
// The game passes adapter=NULL (default adapter) and a params struct.
// params.Version at +0x00 is 0x20004 (SDK 2.4: (2 << 16) | 4 — matches
// the "SDK 2.4" the Series X GDK reports). The struct carries NO
// feature level: on Xbox the level is fixed at 12_2 (DirectX 12
// Ultimate), so the layer takes the best level the desktop hardware
// offers (RX 5700 XT: 12_1). Field offsets beyond Version are from the
// guide's declaration order (Version, GraphicsSwitchingMode,
// ProcessDebugFlags) + the call-site writes (+0x04 = 0,
// +0x08 = 0x100000, +0x20/+0x24 = bytes from globals, +0x28 = 1,
// +0x3c = 0x20); the layer reads Version and logs the rest.
// The old D3D12CreateDevice-shaped (adapter, minFL, riid, ppDevice)
// form was ASSUMED and is now replaced — do not use it.
typedef struct D3D12XBOX_CREATE_DEVICE_PARAMETERS {
    UINT32 Version;                // +0x00: 0x20004 at the real call site
    UINT32 GraphicsSwitchingMode;  // +0x04: 0 (D3D12XBOX_MODE_PROCESS = 0)
    UINT32 ProcessDebugFlags;      // +0x08: 0x100000 at the real call site
    UINT8 reserved[0x50 - 0x0c];   // +0x0c..0x4f (struct is 80 bytes)
} D3D12XBOX_CREATE_DEVICE_PARAMETERS;

HRESULT D3D12XboxCreateDevice(
    IUnknown* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters, REFIID riid,
    void** ppDevice);

// D3D12ConfigureVirtualMemoryX — REAL signature (grounded 2026-11-23
// from the real exe's call site at 0x141e1cfe0): ONE arg, a pointer to
// a config struct (32 bytes, zeroed, struct+4 = a computed value).
// Returns HRESULT; the game hits int3 if it fails, so the layer MUST
// return S_OK (there is no GPU VA space to configure on desktop).
HRESULT D3D12ConfigureVirtualMemoryX(const void* pConfig);

// D3D12SerializeVersionedRootSignature — NOT redeclared here: it is
// already declared in the desktop SDK (d3d12.h), and redeclaring it
// inside this header's extern "C" block conflicts with the SDK
// declaration (C2733). The layer's HleD3D12SerializeVersionedRootSignature
// (src/hle_registry.cpp) calls the SDK function directly; the registry
// maps the Xbox import to that Hle function. No wrapper in
// src/scarlett.cpp either, for the same reason. NOTE: the desktop form
// is 3-arg (pRootSignature, ppBlob, ppErrorBlob) — NO riid (the 4-arg
// riid form belongs to the older non-versioned D3D12SerializeRootSignature);
// the Xbox form is assumed to match (see hle_registry.cpp).

// XMemAllocDefault / XMemFreeDefault — Xbox extended memory API
// (xmem.dll). Same heap as XMemAlloc/XMemFree, no attributes arg.
void* XMemAllocDefault(SIZE_T size);
void XMemFreeDefault(void* p);

// XMemVirtualAlloc — REAL signature (grounded 2026-11-23 from the
// real exe's call site at 0x14193e220):
//   void* XMemVirtualAlloc(void* lpAddress, SIZE_T dwSize,
//       DWORD flAllocationType, DWORD flProtect,
//       DWORD dwMemoryAttributes);
// Returns the allocated VA (NULL on failure). The call site passes
// type 0x20003000 (the same Xbox alloc-type bits as kernelx!
// VirtualAlloc) and attributes 0x2004; protect 0x283 is an Xbox
// protection encoding, not a desktop PAGE_* value.
void* XMemVirtualAlloc(void* lpAddress, SIZE_T dwSize,
                       DWORD flAllocationType, DWORD flProtect,
                       DWORD dwMemoryAttributes);

// XAudio2Create — public XAudio2 2.9 signature (xaudio2.h).
struct IXAudio2;
HRESULT XAudio2Create(IXAudio2** ppXAudio2, UINT32 flags, DWORD processor);

// GameInputCreate — public GameInput API.
struct IGameInput;
HRESULT GameInputCreate(IGameInput** ppGameInput);

// DirectStorage factory — real signature is TWO args: (REFIID riid,
// void** ppFactory), per the desktop DirectStorage API and the Xbox
// XCRDAPI/dstorage_xs versions. Boot #11 proved the 1-arg form wrong:
// the game passes riid in rcx and ppFactory in rdx. The stub ignores
// the riid and serves any IID.
HRESULT DStorageGetFactory(const void* riid, void** ppFactory);

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
