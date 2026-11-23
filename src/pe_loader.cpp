// pe_loader.cpp — Xbox Series X PE32+ loader (scaffolding).
// Same ISA as the host (x86-64), so the guest maps straight into the
// process and its imports are intercepted at the DLL boundary (ScarlettX /
// WinDurango shim-DLL model). No CPU emulation, no JIT.
#include "pe_loader.h"
#include "hle_registry.h"
#include <cstring>
#include <intrin.h>  // __readgsqword
#include <string>
#include <vector>

namespace sc {

namespace {

// Case-insensitive ASCII compare.
bool CiEq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == *b;
}

const IMAGE_SECTION_HEADER* FindSection(const IMAGE_NT_HEADERS64* nt,
                                        const char* name) {
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (strncmp((const char*)sec[i].Name, name, 8) == 0) return &sec[i];
    }
    return nullptr;
}

const uint8_t* RvaToFile(const uint8_t* file, const IMAGE_NT_HEADERS64* nt,
                         uint32_t rva, uint32_t* outSize) {
    if (rva < nt->OptionalHeader.SizeOfHeaders) {
        if (outSize) *outSize = nt->OptionalHeader.SizeOfHeaders - rva;
        return file + rva;
    }
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        uint32_t va = sec[i].VirtualAddress;
        uint32_t sz = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize
                                              : sec[i].SizeOfRawData;
        if (rva >= va && rva < va + sz) {
            uint32_t off = sec[i].PointerToRawData + (rva - va);
            if (outSize) *outSize = sec[i].SizeOfRawData - (rva - va);
            return file + off;
        }
    }
    return nullptr;
}

// Parse "_xbld_edition_name=..." / "_xbld_edition_full_productbuild=..."
// strings out of the .xbld section (Xbox build-info section).
void ParseXbld(const uint8_t* data, size_t size, LoadedPe* out) {
    const char* kName = "_xbld_edition_name=";
    const char* kBuild = "_xbld_edition_full_productbuild=";
    for (size_t i = 0; i + 8 < size;) {
        const char* s = (const char*)data + i;
        size_t len = strnlen(s, size - i);
        if (len == 0) { ++i; continue; }
        if (strncmp(s, kName, strlen(kName)) == 0) {
            strncpy_s(out->xbldEdition, sizeof(out->xbldEdition),
                      s + strlen(kName), _TRUNCATE);
        } else if (strncmp(s, kBuild, strlen(kBuild)) == 0) {
            strncpy_s(out->xbldBuild, sizeof(out->xbldBuild),
                      s + strlen(kBuild), _TRUNCATE);
        }
        i += len + 1;
    }
    out->hasXbld = out->xbldEdition[0] != 0 || out->xbldBuild[0] != 0;
}

// Translate PE section characteristics to a VirtualProtect protection.
DWORD SectionProtection(DWORD characteristics) {
    bool exec = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    bool read = (characteristics & IMAGE_SCN_MEM_READ) != 0;
    bool write = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    DWORD prot;
    if (exec) {
        prot = write ? PAGE_EXECUTE_READWRITE
                     : (read ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    } else if (write) {
        prot = PAGE_READWRITE;
    } else if (read) {
        prot = PAGE_READONLY;
    } else {
        prot = PAGE_NOACCESS;
    }
    if (characteristics & IMAGE_SCN_MEM_NOT_CACHED) prot |= PAGE_NOCACHE;
    return prot;
}

// Apply per-section page protections to the mapped image. Must run AFTER
// relocations + import resolution — both write into the image (relocations
// patch .text/.data, imports write the IAT in .rdata) — so the image is
// RWX until this point. Headers become read-only; each section gets the
// protection its characteristics ask for (text XR, rodata R, data RW).
void ApplySectionProtections(void* base, const IMAGE_NT_HEADERS64* nt) {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const SIZE_T page = si.dwPageSize;
    SIZE_T headers = nt->OptionalHeader.SizeOfHeaders;
    SIZE_T headersRounded = (headers + page - 1) & ~(page - 1);
    {
        DWORD old = 0;
        if (!VirtualProtect(base, headersRounded, PAGE_READONLY, &old))
            Log("loader", "WARNING: header protection failed (%lu)",
                GetLastError());
    }
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        SIZE_T vsize = sec[i].Misc.VirtualSize;
        if (vsize == 0) vsize = sec[i].SizeOfRawData;
        if (vsize == 0) continue;
        char name[9] = {};
        memcpy(name, sec[i].Name, 8);
        SIZE_T start = (SIZE_T)base + sec[i].VirtualAddress;
        // Align the start down to a page, but never below the headers —
        // a section VA is page-aligned in a normal PE, this is defensive.
        SIZE_T alignedStart = start & ~(page - 1);
        if (alignedStart < (SIZE_T)base + headersRounded)
            alignedStart = (SIZE_T)base + headersRounded;
        SIZE_T end = start + vsize;
        SIZE_T alignedEnd = (end + page - 1) & ~(page - 1);
        if (alignedEnd <= alignedStart) continue;
        DWORD prot = SectionProtection(sec[i].Characteristics);
        DWORD old = 0;
        if (!VirtualProtect((void*)alignedStart, alignedEnd - alignedStart,
                            prot, &old)) {
            Log("loader", "WARNING: protection failed for %s (%lu)", name,
                GetLastError());
            continue;
        }
        Log("loader", "protect %s -> 0x%02x (%zu bytes)", name, prot,
            alignedEnd - alignedStart);
    }
}

// Set up TLS for the calling thread: allocate a TLS slot + block, copy
// the template, and run the .CRT$XLB callbacks (DLL_PROCESS_ATTACH).
// The OS normally does this for a loaded module; a manually mapped PE
// gets none of it, so the loader emulates it. Reads the TLS directory
// from the MAPPED image (after relocations — its fields are VAs).
// Runs after protections: the index slot lives in .data (RW), the
// callbacks in .text (X), the template in .tls (R). Best effort: a
// failure logs a warning and the load continues.
bool SetupTls(void* base, const IMAGE_NT_HEADERS64* nt, LoadedPe* out) {
    const IMAGE_DATA_DIRECTORY& tlsDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!tlsDir.VirtualAddress) return true;
    auto* tls = (const IMAGE_TLS_DIRECTORY64*)((uint8_t*)base +
                                               tlsDir.VirtualAddress);
    DWORD index = TlsAlloc();
    if (index == TLS_OUT_OF_INDEXES) {
        Log("loader", "WARNING: TlsAlloc failed (%lu)", GetLastError());
        return false;
    }
    // The guest's __tls_index slot (a VA into the image, relocated).
    DWORD* indexSlot = (DWORD*)tls->AddressOfIndex;
    *indexSlot = index;
    Log("loader",
        "TLS dir: rawdata [%p, %p) indexSlot %p (now %lu) callbacks %p",
        (void*)tls->StartAddressOfRawData,
        (void*)tls->EndAddressOfRawData, (void*)tls->AddressOfIndex,
        *indexSlot, (void*)tls->AddressOfCallBacks);
    SIZE_T templateSize =
        (SIZE_T)(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
    void* block = nullptr;
    if (templateSize > 0) {
        block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, templateSize);
        if (!block) {
            Log("loader", "WARNING: TLS block alloc failed (%lu)",
                GetLastError());
            TlsFree(index);
            return false;
        }
        memcpy(block, (void*)tls->StartAddressOfRawData, templateSize);
        const uint8_t* tb = (const uint8_t*)block;
        Log("loader",
            "TLS template %zu bytes: %02x %02x %02x %02x %02x %02x %02x "
            "%02x",
            templateSize, tb[0], tb[1], tb[2], tb[3], tb[4], tb[5], tb[6],
            tb[7]);
    }
    if (!TlsSetValue(index, block)) {
        Log("loader", "WARNING: TlsSetValue failed (%lu)", GetLastError());
        if (block) HeapFree(GetProcessHeap(), 0, block);
        TlsFree(index);
        return false;
    }
    // Write the block DIRECTLY into the TEB TLS array as well: the
    // compiler's __declspec(thread) access reads TEB->TLS[index] via
    // gs:[58h], and in this process TlsSetValue's slot and the TEB array
    // slot disagree (TlsGetValue returns the block, the direct TEB read
    // returns a stale pointer), so the direct write is what the guest's
    // TLS access actually sees. TlsSetValue is kept for TlsGetValue.
    {
        PVOID* tlsArr = (PVOID*)__readgsqword(0x58);
        tlsArr[index] = block;
    }
    // Confirm the slot actually holds the block (diagnostic: TlsGetValue
    // and the direct TEB read have disagreed in testing).
    {
        void* check = TlsGetValue(index);
        PVOID* tlsArr = (PVOID*)__readgsqword(0x58);
        Log("loader", "TLS slot check: TlsGetValue=%p TEB->TLS[%lu]=%p",
            check, index, tlsArr[index]);
    }
    out->tlsBlock = block;
    out->tlsIndex = index;
    out->tlsIndexValid = true;
    if (tls->AddressOfCallBacks) {
        auto* callbacks = (PIMAGE_TLS_CALLBACK*)tls->AddressOfCallBacks;
        for (size_t i = 0; callbacks[i]; ++i) {
            __try {
                callbacks[i](base, DLL_PROCESS_ATTACH, nullptr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("loader", "WARNING: TLS callback %zu faulted (0x%08x)",
                    i, GetExceptionCode());
                continue;
            }
            Log("loader", "TLS callback %zu ran (PROCESS_ATTACH)", i);
        }
    }
    Log("loader", "TLS: index %lu, template %zu bytes, block %p", index,
        templateSize, block);
    return true;
}

}  // namespace

bool LoadXboxPe(const wchar_t* path, LoadedPe* out, char* err, size_t errSize) {
    memset(out, 0, sizeof(*out));
    // The guest directory becomes the XPackage mount path.
    {
        std::wstring dir = path;
        size_t p = dir.find_last_of(L"\\/");
        if (p != std::wstring::npos) {
            dir = dir.substr(0, p);
            SetGuestDir(dir.c_str());
        }
    }
    auto fail = [&](const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(err, errSize, fmt, ap);
        va_end(ap);
        Log("loader", "FAIL %s", err);
        return false;
    };

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return fail("cannot open %ls (err %lu)", path, GetLastError());
    DWORD fsize = GetFileSize(h, nullptr);
    std::vector<uint8_t> file(fsize);
    DWORD read = 0;
    if (!ReadFile(h, file.data(), fsize, &read, nullptr) || read != fsize) {
        CloseHandle(h);
        return fail("cannot read %ls", path);
    }
    CloseHandle(h);
    const uint8_t* buf = file.data();

    if (fsize < sizeof(IMAGE_DOS_HEADER) ||
        *(const uint16_t*)buf != IMAGE_DOS_SIGNATURE)
        return fail("not a PE (bad MZ)");
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)buf;
    if (dos->e_lfanew < 0 ||
        (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > fsize)
        return fail("bad e_lfanew");
    const IMAGE_NT_HEADERS64* nt =
        (const IMAGE_NT_HEADERS64*)(buf + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return fail("bad PE signature");
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        return fail("not x86-64 (machine 0x%04x)",
                    nt->FileHeader.Machine);
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return fail("not PE32+ (magic 0x%04x)", nt->OptionalHeader.Magic);
    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE))
        return fail("not marked executable");

    out->preferredBase = nt->OptionalHeader.ImageBase;
    Log("loader", "PE ok: machine x64, imagebase 0x%llx, entry 0x%x, "
                  "image size 0x%x, sections %u",
        (unsigned long long)out->preferredBase,
        nt->OptionalHeader.AddressOfEntryPoint,
        nt->OptionalHeader.SizeOfImage, nt->FileHeader.NumberOfSections);

    // .xbld validation (Xbox build-info section).
    const IMAGE_SECTION_HEADER* xbld = FindSection(nt, ".xbld");
    if (xbld) {
        const uint8_t* data = buf + xbld->PointerToRawData;
        ParseXbld(data, xbld->SizeOfRawData, out);
        Log("loader", ".xbld present: edition='%s' build='%s'",
            out->xbldEdition, out->xbldBuild);
    } else {
        Log("loader", "WARNING: no .xbld section (not an Xbox-built PE?)");
    }

    // Map the image.
    SIZE_T imageSize = nt->OptionalHeader.SizeOfImage;
    void* base = VirtualAlloc((void*)out->preferredBase, imageSize,
                              MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    out->relocated = false;
    if (!base) {
        base = VirtualAlloc(nullptr, imageSize, MEM_RESERVE | MEM_COMMIT,
                            PAGE_EXECUTE_READWRITE);
        out->relocated = true;
        Log("loader", "preferred base busy, mapped at %p (relocating)", base);
    }
    if (!base) return fail("VirtualAlloc failed (err %lu)", GetLastError());
    out->base = base;

    // Copy headers + sections.
    memcpy(base, buf, nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].SizeOfRawData == 0) continue;
        memcpy((uint8_t*)base + sec[i].VirtualAddress,
               buf + sec[i].PointerToRawData, sec[i].SizeOfRawData);
    }

    // Base relocations (only needed when we could not use the preferred base).
    if (out->relocated) {
        uint64_t delta = (uint64_t)base - out->preferredBase;
        const IMAGE_DATA_DIRECTORY& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (dir.VirtualAddress && dir.Size) {
            uint8_t* reloc = (uint8_t*)base + dir.VirtualAddress;
            uint8_t* end = reloc + dir.Size;
            while (reloc < end) {
                auto* block = (IMAGE_BASE_RELOCATION*)reloc;
                if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
                uint32_t count =
                    (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
                const uint16_t* entries =
                    (const uint16_t*)(reloc + sizeof(IMAGE_BASE_RELOCATION));
                for (uint32_t j = 0; j < count; ++j) {
                    uint16_t type = entries[j] >> 12;
                    uint16_t off = entries[j] & 0x0FFF;
                    if (type == IMAGE_REL_BASED_DIR64) {
                        uint64_t* patch =
                            (uint64_t*)((uint8_t*)base + block->VirtualAddress + off);
                        *patch += delta;
                    }
                }
                reloc += block->SizeOfBlock;
            }
            Log("loader", "relocations applied (delta 0x%llx)",
                (unsigned long long)delta);
        }
    }

    // TLS directory: noted here; set up after protections (see below).
    const IMAGE_DATA_DIRECTORY& tlsDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    out->hasTls = tlsDir.VirtualAddress != 0;
    if (out->hasTls)
        Log("loader", "TLS directory present");

    // Import resolution.
    const IMAGE_DATA_DIRECTORY& impDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress && impDir.Size) {
        auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)RvaToFile(
            buf, nt, impDir.VirtualAddress, nullptr);
        for (; desc && desc->Name; ++desc) {
            const char* modName =
                (const char*)RvaToFile(buf, nt, desc->Name, nullptr);
            if (!modName) continue;
            out->importModules++;
            bool shim = IsShimModule(modName);
            HMODULE realMod = nullptr;
            if (!shim) {
                realMod = LoadLibraryA(modName);
                if (!realMod)
                    Log("loader", "LoadLibrary failed for %s (err %lu)",
                        modName, GetLastError());
            }
            uint32_t oftRva = desc->OriginalFirstThunk
                                  ? desc->OriginalFirstThunk
                                  : desc->FirstThunk;
            auto* oft = (const IMAGE_THUNK_DATA64*)RvaToFile(buf, nt, oftRva, nullptr);
            auto* ft = (IMAGE_THUNK_DATA64*)((uint8_t*)base + desc->FirstThunk);
            if (!oft || !ft) continue;
            for (size_t k = 0; oft[k].u1.Ordinal != 0; ++k) {
                void* target = nullptr;
                if (oft[k].u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                    // Ordinal import (e.g. d3d12.dll!D3D12CreateDevice is
                    // ordinal 101 in the SDK import lib). Real modules
                    // resolve via GetProcAddress(MAKEINTRESOURCE); shim
                    // modules resolve through the ordinal table
                    // (kOrdinalExports — synthetic ordinals in .def order
                    // until the exe's import table confirms the real ones).
                    uint16_t ord =
                        (uint16_t)(oft[k].u1.Ordinal & 0xFFFF);
                    if (shim) {
                        const OrdinalExport* oe =
                            FindOrdinalExport(modName, ord);
                        if (oe && oe->fn) {
                            target = oe->fn;
                            Log("loader", "ordinal %s:#%u -> %s", modName,
                                ord, oe->name);
                        }
                    } else if (realMod) {
                        target = (void*)GetProcAddress(
                            realMod, MAKEINTRESOURCEA(ord));
                        if (target)
                            Log("loader", "ordinal %s:#%u resolved", modName,
                                ord);
                    }
                    if (!target)
                        Log("loader",
                            "ordinal import %s:#%u unresolved",
                            modName, ord);
                } else {
                    auto* ibn = (const IMAGE_IMPORT_BY_NAME*)RvaToFile(
                        buf, nt, (uint32_t)oft[k].u1.AddressOfData, nullptr);
                    if (ibn) {
                        const char* fnName = (const char*)ibn->Name;
                        if (shim) {
                            const Export* ex = FindExport(modName, fnName);
                            if (ex && ex->fn) {
                                target = ex->fn;
                                Log("loader", "shim  %s!%s -> %s",
                                    modName, fnName,
                                    ex->status == Status::kImplemented
                                        ? "implemented"
                                        : "stub");
                            } else {
                                Log("loader", "shim MISSING %s!%s "
                                              "(no handler, E_NOTIMPL stub)",
                                    modName, fnName);
                            }
                        } else if (realMod) {
                            target = (void*)GetProcAddress(realMod, fnName);
                        }
                    }
                }
                if (target) {
                    ft[k].u1.Function = (uint64_t)target;
                    out->importsResolved++;
                } else {
                    ft[k].u1.Function = (uint64_t)(void*)ScNotImplementedStub;
                    out->importsFailed++;
                }
            }
        }
    }
    Log("loader", "imports: %u modules, %u resolved, %u failed (E_NOTIMPL stub)",
        out->importModules, out->importsResolved, out->importsFailed);

    // Final page protections: per-section (text XR, rodata R, data RW).
    // Applied AFTER relocations + imports, which write into the image.
    ApplySectionProtections(base, nt);
    // TLS setup for this thread (slot + block + callbacks), after
    // protections so the index slot (.data, RW) and callbacks (.text, X)
    // are in their final state.
    if (out->hasTls) SetupTls(base, nt, out);
    out->entry = (uint8_t*)base + nt->OptionalHeader.AddressOfEntryPoint;
    Log("loader", "mapped at %p, entry %p", base, out->entry);
    return true;
}

bool RunXboxPe(LoadedPe* pe, DWORD* exitCode) {
    typedef DWORD(WINAPI* EntryFn)();
    // TLS state just before the entry point (diagnostic).
    if (pe->tlsIndexValid) {
        void* check = TlsGetValue(pe->tlsIndex);
        PVOID* tlsArr = (PVOID*)__readgsqword(0x58);
        Log("loader", "TLS pre-entry: TlsGetValue=%p TEB->TLS[%lu]=%p",
            check, pe->tlsIndex, tlsArr[pe->tlsIndex]);
    }
    __try {
        DWORD rc = ((EntryFn)pe->entry)();
        *exitCode = rc;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exitCode = (DWORD)GetExceptionCode();
        Log("loader", "guest crashed in entry: exception 0x%08x",
            *exitCode);
        return false;
    }
}

void FreeXboxPe(LoadedPe* pe) {
    if (pe->tlsIndexValid) {
        TlsSetValue(pe->tlsIndex, nullptr);
        if (pe->tlsBlock) HeapFree(GetProcessHeap(), 0, pe->tlsBlock);
        TlsFree(pe->tlsIndex);
    }
    if (pe->base) VirtualFree(pe->base, 0, MEM_RELEASE);
    memset(pe, 0, sizeof(*pe));
}

}  // namespace sc
