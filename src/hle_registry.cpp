// hle_registry.cpp — the HLE export registry (Xenia-style) + logging.
// Central table: (Xbox DLL, function) -> host implementation + status tag.
// The loader resolves guest imports through FindExport(); anything missing
// gets the E_NOTIMPL stub. Statuses are the project's tracking surface —
// see docs/STATUS.md.
#include "hle_registry.h"
#include "xbox_shims.h"
#include <mmsystem.h>  // timeBeginPeriod (frame pacing, phase 3)
#include <string.h>
#include <vector>

namespace sc {

namespace {

// ---- logging ----

void WriteLogLine(const char* line) {
    fputs(line, stderr);
    fputc('\n', stderr);
    FILE* f = nullptr;
    if (fopen_s(&f, "scarlett.log", "a") == 0 && f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }
}

}  // namespace

void Log(const char* subsystem, const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[1200];
    snprintf(line, sizeof(line), "[%-10s] %s", subsystem, body);
    WriteLogLine(line);
}

extern "C" HRESULT ScNotImplementedStub() {
    Log("hle", "called unimplemented import -> E_NOTIMPL");
    return 0x80004001;  // E_NOTIMPL
}

// ---- layer-wide state ----

// Device created by D3D12XboxCreateDevice (real desktop device, cached so
// later steps — swapchain, pacing — can use it).
static ID3D12Device* g_xsDevice = nullptr;

// Frame pacing state (real, Sleep-based): the interval set by
// SetFrameIntervalX (1 = 60 Hz) defines the period; ScheduleFrameEventX
// stamps a frame; WaitFrameEventX sleeps until that frame's deadline.
static UINT32 g_frameInterval = 1;
static UINT64 g_lastFrameId = 0;
static LARGE_INTEGER g_lastFrameQpc = {};
static bool g_haveFrame = false;

static double FramePeriodMs() {
    return (double)g_frameInterval * (1000.0 / 60.0);
}

// ---- host implementations ----

extern "C" HRESULT HleXGameRuntimeInitialize() {
    Log("xgrt", "XGameRuntimeInitialize() -> S_OK");
    return S_OK;
}

extern "C" void* HleXMemAlloc(SIZE_T size, DWORD attrs) {
    void* p = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    Log("xmem", "XMemAlloc(%llu, attrs 0x%x) -> %p", (unsigned long long)size,
        attrs, p);
    return p;
}

extern "C" void HleXMemFree(void* p) {
    Log("xmem", "XMemFree(%p)", p);
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

extern "C" HRESULT HleD3D12XboxCreateDevice(IUnknown* adapter,
                                            D3D_FEATURE_LEVEL minFL,
                                            REFIID riid, void** ppDevice) {
    // REAL translation (phase 2): the assumed signature is
    // D3D12CreateDevice-shaped, so this is a thin passthrough to the
    // desktop device. The REAL Xbox signature takes a
    // D3D12XBOX_CREATE_DEVICE_PARAMETERS struct (GDKX-gated) — that form
    // must be confirmed against the exe's import table once it lands.
    // The created device is cached so later steps (swapchain, pacing)
    // can use it.
    HRESULT hr = D3D12CreateDevice(adapter, minFL, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        if (g_xsDevice) g_xsDevice->Release();
        g_xsDevice = (ID3D12Device*)*ppDevice;
        g_xsDevice->AddRef();
        Log("d3d12xs",
            "D3D12XboxCreateDevice -> REAL desktop device %p (minFL 0x%x)",
            *ppDevice, (unsigned)minFL);
    } else {
        Log("d3d12xs",
            "D3D12XboxCreateDevice FAILED hr=0x%08x (minFL 0x%x)",
            (unsigned)hr, (unsigned)minFL);
    }
    return hr;
}

extern "C" HRESULT HleXAudio2Create(void** ppXAudio2, UINT32 flags,
                                    DWORD proc) {
    Log("audio", "XAudio2Create(flags 0x%x, proc %lu) -> S_OK (STUB)",
        flags, proc);
    if (ppXAudio2) *ppXAudio2 = (void*)0xA0D10ULL;
    return S_OK;
}

extern "C" HRESULT HleGameInputCreate(void** ppGameInput) {
    Log("input", "GameInputCreate() -> S_OK (STUB)");
    if (ppGameInput) *ppGameInput = (void*)0x6A6E707574ULL;  // "jnput"
    return S_OK;
}

extern "C" HRESULT HleDStorageGetFactory(void** ppFactory) {
    Log("dstorage", "DStorageGetFactory() -> S_OK (STUB, no queues yet)");
    if (ppFactory) *ppFactory = (void*)0xD570A6EULL;
    return S_OK;
}

extern "C" HRESULT HleSetFrameIntervalX(UINT32 interval, UINT32 flags) {
    if (interval == 0) interval = 1;
    g_frameInterval = interval;
    // Raise the system timer resolution once (phase 3): at the default
    // 15.6 ms clock tick, a waitable timer set for 16.7 ms can fire up
    // to a full tick late (measured 27.7 ms). timeBeginPeriod(1) makes
    // the timer fire within ~1 ms of the due time. Games do this
    // routinely; the resolution is process-wide.
    static bool timerResRaised = false;
    if (!timerResRaised) {
        if (timeBeginPeriod(1) == TIMERR_NOERROR) timerResRaised = true;
    }
    Log("present",
        "SetFrameIntervalX(%u, 0x%x) -> S_OK (real pacing: %.1f ms/frame)",
        interval, flags, FramePeriodMs());
    return S_OK;
}

extern "C" HRESULT HleScheduleFrameEventX(UINT64 frameId) {
    g_lastFrameId = frameId;
    QueryPerformanceCounter(&g_lastFrameQpc);
    g_haveFrame = true;
    Log("present", "ScheduleFrameEventX(%llu) -> S_OK", 
        (unsigned long long)frameId);
    return S_OK;
}

extern "C" HRESULT HleWaitFrameEventX(UINT64 frameId, DWORD timeoutMs) {
    if (!g_haveFrame || frameId != g_lastFrameId) {
        Log("present",
            "WaitFrameEventX(%llu) -> S_OK (no such frame scheduled)",
            (unsigned long long)frameId);
        return S_OK;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsedMs = (double)(now.QuadPart - g_lastFrameQpc.QuadPart) *
                       1000.0 / (double)freq.QuadPart;
    double remainMs = FramePeriodMs() - elapsedMs;
    double waitMs = remainMs < (double)timeoutMs ? remainMs
                                                 : (double)timeoutMs;
    if (waitMs > 0) {
        // Two-phase wait (phase 3): the waitable timer covers all but
        // the last ~1.5 ms (it fires at the system clock granularity),
        // then a QPC spin covers the tail so the wait lands on the
        // deadline instead of overshooting by a tick. The spin is
        // bounded to ~2 ms.
        double timerMs = waitMs - 1.5;
        if (timerMs > 0) {
            HANDLE timer = CreateWaitableTimer(nullptr, TRUE, nullptr);
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)(timerMs * 10000.0);
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr,
                                     FALSE))
                    WaitForSingleObject(timer, (DWORD)(timerMs + 100) + 100);
                CloseHandle(timer);
            }
        }
        LARGE_INTEGER deadline;
        deadline.QuadPart = now.QuadPart +
                            (LONGLONG)(waitMs * (double)freq.QuadPart /
                                       1000.0);
        LARGE_INTEGER spin;
        do {
            QueryPerformanceCounter(&spin);
        } while (spin.QuadPart < deadline.QuadPart);
        QueryPerformanceCounter(&now);
        double actualMs = (double)(now.QuadPart - g_lastFrameQpc.QuadPart) *
                          1000.0 / (double)freq.QuadPart;
        Log("present",
            "WaitFrameEventX(%llu) waited ~%.1f ms (period %.1f ms, "
            "timeout %lu ms)",
            (unsigned long long)frameId, actualMs, FramePeriodMs(),
            timeoutMs);
    } else {
        Log("present",
            "WaitFrameEventX(%llu) already past deadline (%.1f ms elapsed)",
            (unsigned long long)frameId, elapsedMs);
    }
    return S_OK;
}

// ---- XAsync (phase 2, milestone E) ----
// Synchronous completion: the provider does the work inline and completes
// the block before returning, so XAsyncGetStatus reports the final
// HRESULT immediately. internal[0] = HRESULT, [1] = result size,
// [2] = result pointer, [3] = completed flag.

static void CompleteAsync(XAsyncBlock* async, HRESULT hr) {
    async->internal[0] = (void*)(intptr_t)hr;
    async->internal[3] = (void*)1;
    if (async->callback) async->callback(async);
}

static bool AsyncIsComplete(const XAsyncBlock* async) {
    return async->internal[3] != nullptr;
}

extern "C" HRESULT HleXAsyncGetStatus(XAsyncBlock* async, BOOL wait) {
    (void)wait;  // synchronous model: nothing to wait on
    if (!async) return E_INVALIDARG;
    HRESULT hr = (HRESULT)(intptr_t)async->internal[0];
    Log("xasync", "XAsyncGetStatus() -> 0x%08x (%s)", (unsigned)hr,
        AsyncIsComplete(async) ? "complete" : "pending");
    return AsyncIsComplete(async) ? hr : E_PENDING;
}

extern "C" HRESULT HleXAsyncGetResultSize(XAsyncBlock* async, size_t* size) {
    if (!async || !size) return E_INVALIDARG;
    *size = (size_t)async->internal[1];
    Log("xasync", "XAsyncGetResultSize() -> %zu", *size);
    return S_OK;
}

extern "C" HRESULT HleXAsyncCancel(XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    if (!AsyncIsComplete(async))
        CompleteAsync(async, E_ABORT);
    Log("xasync", "XAsyncCancel() -> S_OK");
    return S_OK;
}

extern "C" HRESULT HleXAsyncRun(XAsyncBlock* async, XAsyncWork work) {
    if (!async || !work) return E_INVALIDARG;
    work(async);
    if (!AsyncIsComplete(async)) CompleteAsync(async, S_OK);
    Log("xasync", "XAsyncRun() -> S_OK");
    return S_OK;
}

// ---- XUser (phase 2, milestone E) ----
// Simplified user model: a single synthetic local default user. No real
// Xbox Live sign-in; interactive options are rejected with E_INVALIDARG.

struct XUserImpl {
    uint64_t xuid;
    uint64_t localId;
    char gamertag[16];
};

static XUserImpl g_defaultUser = {0x0009000000000001ULL, 1, "Player"};

extern "C" HRESULT HleXUserAddAsync(XUserAddOptions options,
                                     XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    // Simplified model: only silent works; interactive is rejected.
    if (options & (XUserAddOptions_AddDefaultUserAllowingUI |
                   XUserAddOptions_AllowGuests)) {
        Log("xuser",
            "XUserAddAsync(options 0x%x) -> E_INVALIDARG (interactive "
            "sign-in not supported)",
            (unsigned)options);
        return E_INVALIDARG;
    }
    async->internal[2] = &g_defaultUser;
    async->internal[1] = (void*)sizeof(void*);
    CompleteAsync(async, S_OK);
    Log("xuser", "XUserAddAsync(options 0x%x) -> S_OK (default user)",
        (unsigned)options);
    return S_OK;
}

extern "C" HRESULT HleXUserAddResult(XAsyncBlock* async, XUserHandle* user) {
    if (!async || !user) return E_INVALIDARG;
    HRESULT hr = (HRESULT)(intptr_t)async->internal[0];
    if (FAILED(hr)) {
        Log("xuser", "XUserAddResult() -> 0x%08x", (unsigned)hr);
        return hr;
    }
    *user = (XUserHandle)async->internal[2];
    Log("xuser", "XUserAddResult() -> S_OK user=%p", *user);
    return S_OK;
}

extern "C" HRESULT HleXUserGetId(XUserHandle user, uint64_t* xuid) {
    if (!user || !xuid) return E_INVALIDARG;
    *xuid = ((XUserImpl*)user)->xuid;
    Log("xuser", "XUserGetId() -> 0x%llx", (unsigned long long)*xuid);
    return S_OK;
}

extern "C" HRESULT HleXUserGetGamertag(XUserHandle user,
                                        XUserGamertagOptions options,
                                        char* gamertag, size_t* gamertagSize) {
    (void)options;  // UniqueModern and Classic both return "Player" here.
    if (!user || !gamertagSize) return E_INVALIDARG;
    const char* tag = ((XUserImpl*)user)->gamertag;
    size_t need = strlen(tag) + 1;
    if (!gamertag) {
        *gamertagSize = need;
        return S_OK;
    }
    if (*gamertagSize < need) {
        *gamertagSize = need;
        Log("xuser", "XUserGetGamertag() -> E_NOT_SUFFICIENT_BUFFER");
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    strcpy_s(gamertag, *gamertagSize, tag);
    *gamertagSize = need;
    Log("xuser", "XUserGetGamertag() -> \"%s\"", tag);
    return S_OK;
}

extern "C" HRESULT HleXUserGetLocalId(XUserHandle user, XUserLocalId* localId) {
    if (!user || !localId) return E_INVALIDARG;
    localId->value = ((XUserImpl*)user)->localId;
    Log("xuser", "XUserGetLocalId() -> %llu",
        (unsigned long long)localId->value);
    return S_OK;
}

extern "C" HRESULT HleXUserCloseHandle(XUserHandle user) {
    if (!user) return E_INVALIDARG;
    // The default user is a static singleton — nothing to free.
    Log("xuser", "XUserCloseHandle(%p) -> S_OK", user);
    return S_OK;
}

// ---- XGameSave (phase 2, milestone E) ----
// Local-only provider rooted at
// %LOCALAPPDATA%\ScarlettCompat\XGameSave\<SCID>\. No cloud sync.

struct XGameSaveProviderImpl {
    char rootPath[MAX_PATH];
};

struct XGameSaveContainerImpl {
    char path[MAX_PATH];
};

static bool MakeDirs(const char* path) {
    // Create every component (CreateDirectory is not recursive).
    char tmp[MAX_PATH];
    if (strlen(path) >= sizeof(tmp)) return false;
    strcpy_s(tmp, sizeof(tmp), path);
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            char save = *p;
            *p = 0;
            CreateDirectoryA(tmp, nullptr);
            *p = save;
        }
    }
    return CreateDirectoryA(tmp, nullptr) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

extern "C" HRESULT HleXGameSaveInitializeProvider(XUserHandle user,
                                                   const char* scid,
                                                   BOOL allowSynchronous,
                                                   XGameSaveProviderHandle* provider) {
    (void)allowSynchronous;  // always synchronous / local here
    if (!provider || !scid || !*scid) return E_INVALIDARG;
    // Null user = machine-only provider (local only). A real user is
    // accepted but the save is still local (no cloud sync).
    char localAppData[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData,
                                      sizeof(localAppData));
    char root[MAX_PATH];
    if (n > 0 && n < sizeof(localAppData)) {
        snprintf(root, sizeof(root), "%s\\ScarlettCompat\\XGameSave\\%s",
                 localAppData, scid);
    } else {
        snprintf(root, sizeof(root), ".\\ScarlettCompat\\XGameSave\\%s",
                 scid);
    }
    if (!MakeDirs(root)) {
        Log("xgamesave", "XGameSaveInitializeProvider() -> FAILED to create %s",
            root);
        return E_FAIL;
    }
    XGameSaveProviderImpl* p = (XGameSaveProviderImpl*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*p));
    if (!p) return E_OUTOFMEMORY;
    strcpy_s(p->rootPath, sizeof(p->rootPath), root);
    *provider = p;
    Log("xgamesave",
        "XGameSaveInitializeProvider(user=%p, scid=%s) -> S_OK root=%s",
        user, scid, root);
    return S_OK;
}

extern "C" HRESULT HleXGameSaveCloseProvider(XGameSaveProviderHandle provider) {
    if (!provider) return E_INVALIDARG;
    HeapFree(GetProcessHeap(), 0, provider);
    Log("xgamesave", "XGameSaveCloseProvider() -> S_OK");
    return S_OK;
}

extern "C" HRESULT HleXGameSaveCreateContainer(XGameSaveProviderHandle provider,
                                                const char* displayName,
                                                XGameSaveContainerHandle* container) {
    if (!provider || !displayName || !*displayName || !container)
        return E_INVALIDARG;
    XGameSaveProviderImpl* p = (XGameSaveProviderImpl*)provider;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", p->rootPath, displayName);
    if (!MakeDirs(path)) {
        Log("xgamesave", "XGameSaveCreateContainer(%s) -> FAILED", displayName);
        return E_FAIL;
    }
    XGameSaveContainerImpl* c = (XGameSaveContainerImpl*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c) return E_OUTOFMEMORY;
    strcpy_s(c->path, sizeof(c->path), path);
    *container = c;
    Log("xgamesave", "XGameSaveCreateContainer(%s) -> S_OK path=%s",
        displayName, path);
    return S_OK;
}

// ---- XGameSave update flow (phase 3) ----
// An update stages blob writes/deletes in memory; SubmitUpdate applies
// them to the container folder. Total staged size is capped at
// GS_MAX_BLOB_SIZE (16 MB, GDK docs); one action per blob per update
// (GDK rule); an update cannot be reused after submit (GDK rule).

#define E_GS_UPDATE_TOO_BIG ((HRESULT)0x80830005)

struct XGameSaveUpdateImpl {
    XGameSaveContainerImpl* container;
    char displayName[256];
    struct Action {
        bool isDelete;
        char blobName[256];
        uint8_t* data;  // null for deletes
        size_t size;
    };
    std::vector<Action> actions;
    size_t totalBytes = 0;
    bool submitted = false;
};

static bool ValidBlobName(const char* name) {
    if (!name || !*name || strlen(name) >= 256) return false;
    for (const char* p = name; *p; ++p) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
            *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|')
            return false;
    }
    return true;
}

static XGameSaveUpdateImpl::Action* FindUpdateAction(XGameSaveUpdateImpl* u,
                                                     const char* blobName) {
    for (auto& a : u->actions)
        if (strcmp(a.blobName, blobName) == 0) return &a;
    return nullptr;
}

static bool WriteFileAtomic(const char* path, const uint8_t* data,
                            size_t size) {
    // Write to a temp file in the same folder, then rename over the
    // target — per-blob atomic. (Whole-update atomicity would need a
    // journal; the local model documents this as best-effort.)
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp-%lu", path, GetCurrentProcessId());
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = TRUE;
    if (size > 0)
        ok = WriteFile(h, data, (DWORD)size, &written, nullptr) &&
             written == size;
    CloseHandle(h);
    if (!ok) {
        DeleteFileA(tmp);
        return false;
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        return false;
    }
    return true;
}

extern "C" HRESULT HleXGameSaveCreateUpdate(
    XGameSaveContainerHandle container, const char* containerDisplayName,
    XGameSaveUpdateHandle* update) {
    if (!container || !containerDisplayName || !*containerDisplayName ||
        !update)
        return E_INVALIDARG;
    XGameSaveUpdateImpl* u = (XGameSaveUpdateImpl*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*u));
    if (!u) return E_OUTOFMEMORY;
    u->container = (XGameSaveContainerImpl*)container;
    strcpy_s(u->displayName, sizeof(u->displayName), containerDisplayName);
    *update = u;
    Log("xgamesave", "XGameSaveCreateUpdate(%s) -> S_OK",
        containerDisplayName);
    return S_OK;
}

extern "C" HRESULT HleXGameSaveSubmitBlobWrite(XGameSaveUpdateHandle update,
                                               const char* blobName,
                                               const uint8_t* data,
                                               size_t byteCount) {
    if (!update || !ValidBlobName(blobName) || (!data && byteCount != 0))
        return E_INVALIDARG;
    XGameSaveUpdateImpl* u = (XGameSaveUpdateImpl*)update;
    if (u->submitted) return E_INVALIDARG;  // cannot reuse after submit
    if (FindUpdateAction(u, blobName)) {
        Log("xgamesave",
            "XGameSaveSubmitBlobWrite(%s) -> E_INVALIDARG (already an "
            "action for this blob in this update)",
            blobName);
        return E_INVALIDARG;
    }
    if (byteCount > GS_MAX_BLOB_SIZE ||
        u->totalBytes + byteCount > GS_MAX_BLOB_SIZE) {
        Log("xgamesave", "XGameSaveSubmitBlobWrite(%s, %zu) -> "
                         "E_GS_UPDATE_TOO_BIG",
            blobName, byteCount);
        return E_GS_UPDATE_TOO_BIG;
    }
    uint8_t* copy = nullptr;
    if (byteCount > 0) {
        copy = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, byteCount);
        if (!copy) return E_OUTOFMEMORY;
        memcpy(copy, data, byteCount);
    }
    XGameSaveUpdateImpl::Action a = {};
    a.isDelete = false;
    strcpy_s(a.blobName, sizeof(a.blobName), blobName);
    a.data = copy;
    a.size = byteCount;
    u->actions.push_back(a);
    u->totalBytes += byteCount;
    Log("xgamesave", "XGameSaveSubmitBlobWrite(%s, %zu bytes) -> S_OK "
                     "(staged, total %zu)",
        blobName, byteCount, u->totalBytes);
    return S_OK;
}

extern "C" HRESULT HleXGameSaveSubmitBlobDelete(XGameSaveUpdateHandle update,
                                                const char* blobName) {
    if (!update || !ValidBlobName(blobName)) return E_INVALIDARG;
    XGameSaveUpdateImpl* u = (XGameSaveUpdateImpl*)update;
    if (u->submitted) return E_INVALIDARG;
    if (FindUpdateAction(u, blobName)) {
        Log("xgamesave",
            "XGameSaveSubmitBlobDelete(%s) -> E_INVALIDARG (already an "
            "action for this blob in this update)",
            blobName);
        return E_INVALIDARG;
    }
    XGameSaveUpdateImpl::Action a = {};
    a.isDelete = true;
    strcpy_s(a.blobName, sizeof(a.blobName), blobName);
    u->actions.push_back(a);
    Log("xgamesave", "XGameSaveSubmitBlobDelete(%s) -> S_OK (staged)",
        blobName);
    return S_OK;
}

static HRESULT ApplyGameSaveUpdate(XGameSaveUpdateImpl* u) {
    if (!u || u->submitted) return E_INVALIDARG;
    for (auto& a : u->actions) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", u->container->path,
                 a.blobName);
        if (a.isDelete) {
            if (!DeleteFileA(path) &&
                GetLastError() != ERROR_FILE_NOT_FOUND) {
                Log("xgamesave",
                    "XGameSaveSubmitUpdate: delete %s failed (%lu)",
                    a.blobName, GetLastError());
                return E_FAIL;
            }
            Log("xgamesave", "XGameSaveSubmitUpdate: deleted blob %s",
                a.blobName);
        } else {
            if (!WriteFileAtomic(path, a.data, a.size)) {
                Log("xgamesave",
                    "XGameSaveSubmitUpdate: write %s failed (%lu)",
                    a.blobName, GetLastError());
                return E_FAIL;
            }
            Log("xgamesave", "XGameSaveSubmitUpdate: wrote blob %s (%zu "
                             "bytes)",
                a.blobName, a.size);
        }
    }
    u->submitted = true;
    return S_OK;
}

extern "C" HRESULT HleXGameSaveSubmitUpdate(XGameSaveUpdateHandle update) {
    if (!update) return E_INVALIDARG;
    HRESULT hr = ApplyGameSaveUpdate((XGameSaveUpdateImpl*)update);
    Log("xgamesave", "XGameSaveSubmitUpdate() -> 0x%08x", (unsigned)hr);
    return hr;
}

extern "C" HRESULT HleXGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle update,
                                                 XAsyncBlock* async) {
    if (!update || !async) return E_INVALIDARG;
    HRESULT hr = ApplyGameSaveUpdate((XGameSaveUpdateImpl*)update);
    CompleteAsync(async, hr);
    Log("xgamesave", "XGameSaveSubmitUpdateAsync() -> 0x%08x (sync "
                     "completion)",
        (unsigned)hr);
    return hr;
}

extern "C" HRESULT HleXGameSaveSubmitUpdateResult(XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    if (!AsyncIsComplete(async)) return E_PENDING;
    HRESULT hr = (HRESULT)(intptr_t)async->internal[0];
    Log("xgamesave", "XGameSaveSubmitUpdateResult() -> 0x%08x",
        (unsigned)hr);
    return hr;
}

extern "C" HRESULT HleXGameSaveCloseUpdate(XGameSaveUpdateHandle update) {
    if (!update) return E_INVALIDARG;
    XGameSaveUpdateImpl* u = (XGameSaveUpdateImpl*)update;
    for (auto& a : u->actions)
        if (a.data) HeapFree(GetProcessHeap(), 0, a.data);
    HeapFree(GetProcessHeap(), 0, u);
    Log("xgamesave", "XGameSaveCloseUpdate() -> S_OK");
    return S_OK;
}

extern "C" HRESULT HleXGameSaveCloseContainer(
    XGameSaveContainerHandle container) {
    if (!container) return E_INVALIDARG;
    HeapFree(GetProcessHeap(), 0, container);
    Log("xgamesave", "XGameSaveCloseContainer() -> S_OK");
    return S_OK;
}

extern "C" HRESULT HleXGameSaveReadBlobData(XGameSaveContainerHandle container,
                                            const char* blobName,
                                            size_t* bytesRead,
                                            size_t bufferSize,
                                            uint8_t* buffer) {
    if (!container || !ValidBlobName(blobName) || !bytesRead)
        return E_INVALIDARG;
    XGameSaveContainerImpl* c = (XGameSaveContainerImpl*)container;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", c->path, blobName);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("xgamesave", "XGameSaveReadBlobData(%s) -> file not found",
            blobName);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    LARGE_INTEGER fsize = {};
    if (!GetFileSizeEx(h, &fsize)) {
        CloseHandle(h);
        return E_FAIL;
    }
    *bytesRead = (size_t)fsize.QuadPart;
    if (bufferSize < *bytesRead) {
        CloseHandle(h);
        Log("xgamesave",
            "XGameSaveReadBlobData(%s) -> E_NOT_SUFFICIENT_BUFFER "
            "(need %zu, have %zu)",
            blobName, *bytesRead, bufferSize);
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if (*bytesRead > 0) {
        if (!buffer) {
            CloseHandle(h);
            return E_INVALIDARG;
        }
        DWORD read = 0;
        if (!ReadFile(h, buffer, (DWORD)*bytesRead, &read, nullptr) ||
            read != *bytesRead) {
            CloseHandle(h);
            return E_FAIL;
        }
    }
    CloseHandle(h);
    Log("xgamesave", "XGameSaveReadBlobData(%s) -> S_OK %zu bytes",
        blobName, *bytesRead);
    return S_OK;
}

// ---- XGameSaveFiles (phase 3) ----
// File-style saves: the async call returns the provider folder; the
// title uses plain Win32 FileIO under it. Root:
// %LOCALAPPDATA%\ScarlettCompat\XGameSaveFiles\<SCID>\. No UI is shown
// headless; no cloud sync (local only).

extern "C" HRESULT HleXGameSaveFilesGetFolderWithUiAsync(
    XUserHandle requestingUser, const char* configurationId,
    XAsyncBlock* async) {
    (void)requestingUser;  // accepted; the folder is per-SCID, not per-user
    if (!async || !configurationId || !*configurationId)
        return E_INVALIDARG;
    char localAppData[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData,
                                      sizeof(localAppData));
    char root[MAX_PATH];
    if (n > 0 && n < sizeof(localAppData)) {
        snprintf(root, sizeof(root),
                 "%s\\ScarlettCompat\\XGameSaveFiles\\%s", localAppData,
                 configurationId);
    } else {
        snprintf(root, sizeof(root),
                 ".\\ScarlettCompat\\XGameSaveFiles\\%s", configurationId);
    }
    if (!MakeDirs(root)) {
        Log("xgamesavefiles",
            "XGameSaveFilesGetFolderWithUiAsync() -> FAILED to create %s",
            root);
        return E_FAIL;
    }
    size_t len = strlen(root) + 1;
    char* stored = (char*)HeapAlloc(GetProcessHeap(), 0, len);
    if (!stored) return E_OUTOFMEMORY;
    strcpy_s(stored, len, root);
    async->internal[2] = stored;
    async->internal[1] = (void*)len;
    CompleteAsync(async, S_OK);
    Log("xgamesavefiles",
        "XGameSaveFilesGetFolderWithUiAsync(scid=%s) -> S_OK folder=%s",
        configurationId, root);
    return S_OK;
}

extern "C" HRESULT HleXGameSaveFilesGetFolderWithUiResult(
    XAsyncBlock* async, size_t bufferSize, char* folder) {
    if (!async || !folder) return E_INVALIDARG;
    if (!AsyncIsComplete(async)) return E_PENDING;
    HRESULT hr = (HRESULT)(intptr_t)async->internal[0];
    if (FAILED(hr)) return hr;
    const char* path = (const char*)async->internal[2];
    if (!path) return E_FAIL;
    size_t need = strlen(path) + 1;
    if (bufferSize < need) {
        Log("xgamesavefiles",
            "XGameSaveFilesGetFolderWithUiResult() -> "
            "E_NOT_SUFFICIENT_BUFFER (need %zu, have %zu)",
            need, bufferSize);
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    strcpy_s(folder, bufferSize, path);
    HeapFree(GetProcessHeap(), 0, (void*)path);
    async->internal[2] = nullptr;
    Log("xgamesavefiles", "XGameSaveFilesGetFolderWithUiResult() -> \"%s\"",
        folder);
    return S_OK;
}

// ---- XPackage (phase 2, milestone E) ----
// Mount path = the directory holding the guest PE (set by the loader).

static wchar_t g_guestDir[MAX_PATH] = {};

void SetGuestDir(const wchar_t* dir) {
    if (!dir) return;
    wcsncpy_s(g_guestDir, sizeof(g_guestDir) / sizeof(wchar_t), dir,
              _TRUNCATE);
    Log("xpackage", "guest dir set to %ls", g_guestDir);
}

extern "C" HRESULT HleXPackageGetMountPathSize(uint32_t* size) {
    if (!size) return E_INVALIDARG;
    *size = (uint32_t)(wcslen(g_guestDir) + 1);
    Log("xpackage", "XPackageGetMountPathSize() -> %lu", *size);
    return S_OK;
}

extern "C" HRESULT HleXPackageGetMountPath(wchar_t* path, uint32_t* pathSize) {
    if (!pathSize) return E_INVALIDARG;
    uint32_t need = (uint32_t)(wcslen(g_guestDir) + 1);
    if (!path) {
        *pathSize = need;
        return S_OK;
    }
    if (*pathSize < need) {
        *pathSize = need;
        Log("xpackage", "XPackageGetMountPath() -> E_NOT_SUFFICIENT_BUFFER");
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    wcscpy_s(path, *pathSize, g_guestDir);
    *pathSize = need;
    Log("xpackage", "XPackageGetMountPath() -> \"%ls\"", g_guestDir);
    return S_OK;
}

extern "C" BOOL HleXPackageIsPackagedProcess(void) {
    // The layer presents the guest as a packaged Game Core title.
    Log("xpackage", "XPackageIsPackagedProcess() -> TRUE");
    return TRUE;
}

// ---- XPackage details (phase 3) ----
// The layer presents a single synthetic package: the guest itself.
// Identifier is synthetic (local-only, no store install); the TitleId
// embedded in it is the GTA VI Scarlett TitleId from the package
// file map, for traceability.

static const char kPackageId[] = "ScarlettCompat-Guest-7952987A";
static const char kPackageDisplayName[] = "ScarlettCompat Guest";
static const char kPackageDescription[] =
    "Synthetic package presented by the ScarlettCompat layer "
    "(local-only, no store install)";
static const char kPackagePublisher[] = "ScarlettCompat";
static const char kPackageTitleId[] = "7952987A";

struct XPackageMountImpl {
    char identifier[XPACKAGE_IDENTIFIER_MAX_LENGTH];
    wchar_t path[MAX_PATH];  // mount path = guest dir
};

extern "C" HRESULT HleXPackageGetCurrentProcessPackageIdentifier(
    size_t bufferSize, char* buffer) {
    if (!buffer) return E_INVALIDARG;
    size_t need = strlen(kPackageId) + 1;
    if (bufferSize < need) {
        Log("xpackage",
            "XPackageGetCurrentProcessPackageIdentifier() -> "
            "E_NOT_SUFFICIENT_BUFFER (need %zu, have %zu)",
            need, bufferSize);
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    strcpy_s(buffer, bufferSize, kPackageId);
    Log("xpackage",
        "XPackageGetCurrentProcessPackageIdentifier() -> \"%s\"",
        kPackageId);
    return S_OK;
}

extern "C" HRESULT HleXPackageEnumeratePackages(
    XPackageKind kind, XPackageEnumerationScope scope, void* context,
    XPackageEnumerationCallback* callback) {
    (void)scope;  // one package: every scope returns the same one
    if (!callback) return E_INVALIDARG;
    if (kind == XPackageKind_Content) {
        Log("xpackage",
            "XPackageEnumeratePackages(Content) -> S_OK, 0 packages");
        return S_OK;
    }
    XPackageDetails d = {};
    d.packageIdentifier = kPackageId;
    d.version.major = 1;
    d.version.minor = 0;
    d.version.build = 0;
    d.version.revision = 0;
    d.kind = XPackageKind_Game;
    d.displayName = kPackageDisplayName;
    d.description = kPackageDescription;
    d.publisher = kPackagePublisher;
    d.storeId = "";
    d.installing = false;
    d.index = 0;
    d.count = 1;
    d.ageRestricted = false;
    d.titleID = kPackageTitleId;
    bool cont = (*callback)(context, &d);
    Log("xpackage",
        "XPackageEnumeratePackages(Game) -> S_OK, 1 package \"%s\" "
        "(continue=%d)",
        kPackageId, (int)cont);
    return S_OK;
}

extern "C" HRESULT HleXPackageMountWithUiAsync(
    const char* packageIdentifier, XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    // No UI is shown headless. The only package that exists locally is
    // the current one; NULL/empty identifier means the current package.
    if (packageIdentifier && *packageIdentifier &&
        strcmp(packageIdentifier, kPackageId) != 0) {
        Log("xpackage",
            "XPackageMountWithUiAsync(\"%s\") -> E_INVALIDARG (unknown "
            "package)",
            packageIdentifier);
        return E_INVALIDARG;
    }
    XPackageMountImpl* m = (XPackageMountImpl*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*m));
    if (!m) return E_OUTOFMEMORY;
    strcpy_s(m->identifier, sizeof(m->identifier), kPackageId);
    wcsncpy_s(m->path, sizeof(m->path) / sizeof(wchar_t), g_guestDir,
              _TRUNCATE);
    async->internal[2] = m;
    async->internal[1] = (void*)sizeof(void*);
    CompleteAsync(async, S_OK);
    Log("xpackage",
        "XPackageMountWithUiAsync() -> S_OK (mount=%p path=%ls)",
        m, m->path);
    return S_OK;
}

extern "C" HRESULT HleXPackageMountWithUiResult(
    XAsyncBlock* async, XPackageMountHandle* mount) {
    if (!async || !mount) return E_INVALIDARG;
    if (!AsyncIsComplete(async)) return E_PENDING;
    HRESULT hr = (HRESULT)(intptr_t)async->internal[0];
    if (FAILED(hr)) {
        Log("xpackage", "XPackageMountWithUiResult() -> 0x%08x",
            (unsigned)hr);
        return hr;
    }
    *mount = (XPackageMountHandle)async->internal[2];
    Log("xpackage", "XPackageMountWithUiResult() -> S_OK mount=%p", *mount);
    return S_OK;
}

extern "C" HRESULT HleXPackageCloseMountHandle(XPackageMountHandle mount) {
    if (!mount) return E_INVALIDARG;
    HeapFree(GetProcessHeap(), 0, mount);
    Log("xpackage", "XPackageCloseMountHandle(%p) -> S_OK", mount);
    return S_OK;
}

// ---- the registry table ----
// Status meanings: kImplemented = real host code; kStub = logs + plausible
// success; kNotImplemented = no handler yet (loader writes E_NOTIMPL stub).

static const Export kExports[] = {
    // Runtime init (boot-critical).
    {"xgameruntime.dll", "XGameRuntimeInitialize", Status::kStub,
     (void*)HleXGameRuntimeInitialize, "First GDK call. Stub returns S_OK."},
    // Memory.
    {"xmem.dll", "XMemAlloc", Status::kImplemented, (void*)HleXMemAlloc,
     "Real: HeapAlloc."},
    {"xmem.dll", "XMemFree", Status::kImplemented, (void*)HleXMemFree,
     "Real: HeapFree."},
    // Graphics.
    {"d3d12_xs.dll", "D3D12XboxCreateDevice", Status::kImplemented,
     (void*)HleD3D12XboxCreateDevice,
     "IMPLEMENTED (phase 2). Real passthrough to desktop D3D12CreateDevice; "
     "signature is D3D12CreateDevice-shaped (assumed) — the real Xbox form "
     "takes D3D12XBOX_CREATE_DEVICE_PARAMETERS; confirm vs exe."},
    {"d3d12_xs.dll", "SetFrameIntervalX", Status::kImplemented,
     (void*)HleSetFrameIntervalX,
     "IMPLEMENTED (phase 2). Real pacing: stores the interval."},
    {"d3d12_xs.dll", "ScheduleFrameEventX", Status::kImplemented,
     (void*)HleScheduleFrameEventX,
     "IMPLEMENTED (phase 2). Real pacing: stamps the frame."},
    {"d3d12_xs.dll", "WaitFrameEventX", Status::kImplemented,
     (void*)HleWaitFrameEventX,
     "IMPLEMENTED (phase 2). Real pacing: sleeps to the frame deadline."},
    // Audio.
    {"XAudio2_9.dll", "XAudio2Create", Status::kStub, (void*)HleXAudio2Create,
     "STUB. XMA2 decode is 0% everywhere; stub only."},
    // Input.
    {"gameinput.dll", "GameInputCreate", Status::kStub,
     (void*)HleGameInputCreate, "STUB."},
    // Storage.
    {"dstorage_xs.dll", "DStorageGetFactory", Status::kStub,
     (void*)HleDStorageGetFactory,
     "STUB. No queues/files yet; world streamer needs real I/O."},
    // Async (phase 2, milestone E): synchronous completion model.
    {"xgameruntime.dll", "XAsyncGetStatus", Status::kImplemented,
     (void*)HleXAsyncGetStatus, "IMPLEMENTED (phase 2). Sync completion."},
    {"xgameruntime.dll", "XAsyncGetResultSize", Status::kImplemented,
     (void*)HleXAsyncGetResultSize, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XAsyncCancel", Status::kImplemented,
     (void*)HleXAsyncCancel, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XAsyncRun", Status::kImplemented,
     (void*)HleXAsyncRun, "IMPLEMENTED (phase 2). Runs work synchronously."},
    // User model (phase 2, milestone E): synthetic default user.
    {"xgameruntime.dll", "XUserAddAsync", Status::kImplemented,
     (void*)HleXUserAddAsync,
     "IMPLEMENTED (phase 2). Simplified model: silent default user only."},
    {"xgameruntime.dll", "XUserAddResult", Status::kImplemented,
     (void*)HleXUserAddResult, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XUserGetId", Status::kImplemented,
     (void*)HleXUserGetId, "IMPLEMENTED (phase 2). Synthetic Xuid."},
    {"xgameruntime.dll", "XUserGetGamertag", Status::kImplemented,
     (void*)HleXUserGetGamertag,
     "IMPLEMENTED (phase 2). Returns \"Player\"."},
    {"xgameruntime.dll", "XUserGetLocalId", Status::kImplemented,
     (void*)HleXUserGetLocalId, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XUserCloseHandle", Status::kImplemented,
     (void*)HleXUserCloseHandle, "IMPLEMENTED (phase 2)."},
    // GameSave (phase 2, milestone E): local-only provider.
    {"xgameruntime.dll", "XGameSaveInitializeProvider", Status::kImplemented,
     (void*)HleXGameSaveInitializeProvider,
     "IMPLEMENTED (phase 2). Local folder under %LOCALAPPDATA%; no cloud."},
    {"xgameruntime.dll", "XGameSaveCloseProvider", Status::kImplemented,
     (void*)HleXGameSaveCloseProvider, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XGameSaveCreateContainer", Status::kImplemented,
     (void*)HleXGameSaveCreateContainer,
     "IMPLEMENTED (phase 2). Container = subfolder of the provider root."},
    // GameSave update flow (phase 3): staged blob writes/deletes.
    {"xgameruntime.dll", "XGameSaveCreateUpdate", Status::kImplemented,
     (void*)HleXGameSaveCreateUpdate,
     "IMPLEMENTED (phase 3). Creates a staged update for a container."},
    {"xgameruntime.dll", "XGameSaveSubmitBlobWrite", Status::kImplemented,
     (void*)HleXGameSaveSubmitBlobWrite,
     "IMPLEMENTED (phase 3). Stages a blob write (16 MB cap)."},
    {"xgameruntime.dll", "XGameSaveSubmitBlobDelete", Status::kImplemented,
     (void*)HleXGameSaveSubmitBlobDelete,
     "IMPLEMENTED (phase 3). Stages a blob delete."},
    {"xgameruntime.dll", "XGameSaveSubmitUpdate", Status::kImplemented,
     (void*)HleXGameSaveSubmitUpdate,
     "IMPLEMENTED (phase 3). Applies the staged update to disk."},
    {"xgameruntime.dll", "XGameSaveSubmitUpdateAsync", Status::kImplemented,
     (void*)HleXGameSaveSubmitUpdateAsync,
     "IMPLEMENTED (phase 3). Applies inline, sync completion."},
    {"xgameruntime.dll", "XGameSaveSubmitUpdateResult", Status::kImplemented,
     (void*)HleXGameSaveSubmitUpdateResult, "IMPLEMENTED (phase 3)."},
    {"xgameruntime.dll", "XGameSaveCloseUpdate", Status::kImplemented,
     (void*)HleXGameSaveCloseUpdate, "IMPLEMENTED (phase 3)."},
    {"xgameruntime.dll", "XGameSaveCloseContainer", Status::kImplemented,
     (void*)HleXGameSaveCloseContainer, "IMPLEMENTED (phase 3)."},
    {"xgameruntime.dll", "XGameSaveReadBlobData", Status::kImplemented,
     (void*)HleXGameSaveReadBlobData,
     "IMPLEMENTED (phase 3). Reads a blob back from the container."},
    // GameSaveFiles (phase 3): file-style saves.
    {"xgameruntime.dll", "XGameSaveFilesGetFolderWithUiAsync",
     Status::kImplemented, (void*)HleXGameSaveFilesGetFolderWithUiAsync,
     "IMPLEMENTED (phase 3). Returns the local provider folder; no UI "
     "headless."},
    {"xgameruntime.dll", "XGameSaveFilesGetFolderWithUiResult",
     Status::kImplemented, (void*)HleXGameSaveFilesGetFolderWithUiResult,
     "IMPLEMENTED (phase 3)."},
    // Package (phase 2, milestone E): mount path = guest dir.
    {"xgameruntime.dll", "XPackageGetMountPathSize", Status::kImplemented,
     (void*)HleXPackageGetMountPathSize, "IMPLEMENTED (phase 2)."},
    {"xgameruntime.dll", "XPackageGetMountPath", Status::kImplemented,
     (void*)HleXPackageGetMountPath,
     "IMPLEMENTED (phase 2). Returns the guest directory."},
    {"xgameruntime.dll", "XPackageIsPackagedProcess", Status::kImplemented,
     (void*)HleXPackageIsPackagedProcess,
     "IMPLEMENTED (phase 2). Presents the guest as packaged."},
    {"xgameruntime.dll", "XPackageGetCurrentProcessPackageIdentifier",
     Status::kImplemented,
     (void*)HleXPackageGetCurrentProcessPackageIdentifier,
     "IMPLEMENTED (phase 3). Returns the synthetic package identifier. "
     "Signature corrected to the real (bufferSize, char*) form."},
    {"xgameruntime.dll", "XPackageEnumeratePackages", Status::kImplemented,
     (void*)HleXPackageEnumeratePackages,
     "IMPLEMENTED (phase 3). Enumerates the single synthetic package."},
    {"xgameruntime.dll", "XPackageMountWithUiAsync", Status::kImplemented,
     (void*)HleXPackageMountWithUiAsync,
     "IMPLEMENTED (phase 3). Mounts the current package; no UI headless."},
    {"xgameruntime.dll", "XPackageMountWithUiResult", Status::kImplemented,
     (void*)HleXPackageMountWithUiResult, "IMPLEMENTED (phase 3)."},
    {"xgameruntime.dll", "XPackageCloseMountHandle", Status::kImplemented,
     (void*)HleXPackageCloseMountHandle, "IMPLEMENTED (phase 3)."},
};

static const char* kShimModules[] = {
    "xgameruntime.dll", "xgameplatform.dll", "d3d12_xs.dll", "d3d12_x.dll",
    "xmem.dll", "XAudio2_9.dll", "dstorage_xs.dll", "gameinput.dll",
    "xgame_save.dll", "xstore.dll",
};

static bool CiEq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == *b;
}

const Export* FindExport(const char* module, const char* name) {
    for (size_t i = 0; i < sizeof(kExports) / sizeof(kExports[0]); ++i) {
        if (CiEq(kExports[i].module, module) && CiEq(kExports[i].name, name))
            return &kExports[i];
    }
    return nullptr;
}

// Ordinal table (synthetic ordinals — see scarlett.h). One entry per
// registered export, numbered in .def order per module.
static const OrdinalExport kOrdinalExports[] = {
    {"xgameruntime.dll", 1, (void*)HleXGameRuntimeInitialize,
     "XGameRuntimeInitialize"},
    {"xgameruntime.dll", 2, (void*)HleXAsyncGetStatus, "XAsyncGetStatus"},
    {"xgameruntime.dll", 3, (void*)HleXAsyncGetResultSize,
     "XAsyncGetResultSize"},
    {"xgameruntime.dll", 4, (void*)HleXAsyncCancel, "XAsyncCancel"},
    {"xgameruntime.dll", 5, (void*)HleXAsyncRun, "XAsyncRun"},
    {"xgameruntime.dll", 6, (void*)HleXUserAddAsync, "XUserAddAsync"},
    {"xgameruntime.dll", 7, (void*)HleXUserAddResult, "XUserAddResult"},
    {"xgameruntime.dll", 8, (void*)HleXUserGetId, "XUserGetId"},
    {"xgameruntime.dll", 9, (void*)HleXUserGetGamertag, "XUserGetGamertag"},
    {"xgameruntime.dll", 10, (void*)HleXUserGetLocalId, "XUserGetLocalId"},
    {"xgameruntime.dll", 11, (void*)HleXUserCloseHandle, "XUserCloseHandle"},
    {"xgameruntime.dll", 12, (void*)HleXGameSaveInitializeProvider,
     "XGameSaveInitializeProvider"},
    {"xgameruntime.dll", 13, (void*)HleXGameSaveCloseProvider,
     "XGameSaveCloseProvider"},
    {"xgameruntime.dll", 14, (void*)HleXGameSaveCreateContainer,
     "XGameSaveCreateContainer"},
    {"xgameruntime.dll", 15, (void*)HleXPackageGetMountPathSize,
     "XPackageGetMountPathSize"},
    {"xgameruntime.dll", 16, (void*)HleXPackageGetMountPath,
     "XPackageGetMountPath"},
    {"xgameruntime.dll", 17, (void*)HleXPackageIsPackagedProcess,
     "XPackageIsPackagedProcess"},
    {"xgameruntime.dll", 18,
     (void*)HleXPackageGetCurrentProcessPackageIdentifier,
     "XPackageGetCurrentProcessPackageIdentifier"},
    {"xgameruntime.dll", 19, (void*)HleXPackageMountWithUiAsync,
     "XPackageMountWithUiAsync"},
    {"xgameruntime.dll", 20, (void*)HleXPackageMountWithUiResult,
     "XPackageMountWithUiResult"},
    {"xgameruntime.dll", 21, (void*)HleXPackageCloseMountHandle,
     "XPackageCloseMountHandle"},
    // Phase 3: GameSave update flow + GameSaveFiles + package details.
    {"xgameruntime.dll", 22, (void*)HleXGameSaveCreateUpdate,
     "XGameSaveCreateUpdate"},
    {"xgameruntime.dll", 23, (void*)HleXGameSaveSubmitBlobWrite,
     "XGameSaveSubmitBlobWrite"},
    {"xgameruntime.dll", 24, (void*)HleXGameSaveSubmitBlobDelete,
     "XGameSaveSubmitBlobDelete"},
    {"xgameruntime.dll", 25, (void*)HleXGameSaveSubmitUpdate,
     "XGameSaveSubmitUpdate"},
    {"xgameruntime.dll", 26, (void*)HleXGameSaveSubmitUpdateAsync,
     "XGameSaveSubmitUpdateAsync"},
    {"xgameruntime.dll", 27, (void*)HleXGameSaveSubmitUpdateResult,
     "XGameSaveSubmitUpdateResult"},
    {"xgameruntime.dll", 28, (void*)HleXGameSaveCloseUpdate,
     "XGameSaveCloseUpdate"},
    {"xgameruntime.dll", 29, (void*)HleXGameSaveCloseContainer,
     "XGameSaveCloseContainer"},
    {"xgameruntime.dll", 30, (void*)HleXGameSaveReadBlobData,
     "XGameSaveReadBlobData"},
    {"xgameruntime.dll", 31,
     (void*)HleXGameSaveFilesGetFolderWithUiAsync,
     "XGameSaveFilesGetFolderWithUiAsync"},
    {"xgameruntime.dll", 32,
     (void*)HleXGameSaveFilesGetFolderWithUiResult,
     "XGameSaveFilesGetFolderWithUiResult"},
    {"xgameruntime.dll", 33, (void*)HleXPackageEnumeratePackages,
     "XPackageEnumeratePackages"},
    {"xmem.dll", 1, (void*)HleXMemAlloc, "XMemAlloc"},
    {"xmem.dll", 2, (void*)HleXMemFree, "XMemFree"},
    {"d3d12_xs.dll", 1, (void*)HleD3D12XboxCreateDevice,
     "D3D12XboxCreateDevice"},
    {"d3d12_xs.dll", 2, (void*)HleSetFrameIntervalX, "SetFrameIntervalX"},
    {"d3d12_xs.dll", 3, (void*)HleScheduleFrameEventX,
     "ScheduleFrameEventX"},
    {"d3d12_xs.dll", 4, (void*)HleWaitFrameEventX, "WaitFrameEventX"},
    {"XAudio2_9.dll", 1, (void*)HleXAudio2Create, "XAudio2Create"},
    {"gameinput.dll", 1, (void*)HleGameInputCreate, "GameInputCreate"},
    {"dstorage_xs.dll", 1, (void*)HleDStorageGetFactory,
     "DStorageGetFactory"},
};

const OrdinalExport* FindOrdinalExport(const char* module, uint16_t ordinal) {
    for (size_t i = 0;
         i < sizeof(kOrdinalExports) / sizeof(kOrdinalExports[0]); ++i) {
        if (CiEq(kOrdinalExports[i].module, module) &&
            kOrdinalExports[i].ordinal == ordinal)
            return &kOrdinalExports[i];
    }
    return nullptr;
}

size_t ExportCount() { return sizeof(kExports) / sizeof(kExports[0]); }

const Export* ExportAt(size_t i) {
    return i < ExportCount() ? &kExports[i] : nullptr;
}

bool IsShimModule(const char* module) {
    for (size_t i = 0; i < sizeof(kShimModules) / sizeof(kShimModules[0]);
         ++i) {
        if (CiEq(kShimModules[i], module)) return true;
    }
    return false;
}

}  // namespace sc
