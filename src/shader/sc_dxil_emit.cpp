// ScarlettCompat shader pipeline - DXIL emission via dxc.exe (MIT, clean-room).
//
// The emitter is a subprocess driver on purpose: it needs no DXC SDK headers,
// no COM GUIDs and no in-process dependency, so the module stays replaceable
// and the rest of the pipeline never links against the compiler.
#include "shader/sc_dxil_emit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sc {
namespace {

constexpr size_t kMaxLogBytes = 1u << 20; // 1 MiB of compiler output is plenty

#ifdef _WIN32

std::string env_value(const char* name) {
    char buf[2048];
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return std::string();
    }
    return std::string(buf, n);
}

bool is_file(const std::string& path) {
    const DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool run_capture(const std::string& command_line, std::string& out_text, unsigned long& exit_code) {
    exit_code = 1;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        return false;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = nullptr;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::string mutable_cmd = command_line;
    const BOOL created = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        return false;
    }

    out_text.clear();
    char buffer[4096];
    for (;;) {
        DWORD got = 0;
        const BOOL ok = ReadFile(read_pipe, buffer, sizeof(buffer), &got, nullptr);
        if (!ok || got == 0) {
            break;
        }
        out_text.append(buffer, got);
        if (out_text.size() >= kMaxLogBytes) {
            out_text += "\n[log truncated]";
            break;
        }
    }

    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(read_pipe);
    exit_code = code;
    return true;
}

std::string quote(const std::string& s) { return std::string("\"") + s + "\""; }

std::string temp_dir() {
    char buf[MAX_PATH + 1];
    const DWORD n = GetTempPathA(static_cast<DWORD>(sizeof(buf)), buf);
    if (n == 0 || n > sizeof(buf)) {
        return std::string(".");
    }
    return std::string(buf, n);
}

// Collects dxc.exe candidates in priority order.
void collect_candidates(std::vector<std::string>& out) {
    const char* env_names[] = {"SC_DXC_PATH", "DXC_PATH"};
    for (const char* name : env_names) {
        const std::string v = env_value(name);
        if (!v.empty()) {
            out.push_back(v);
        }
    }

    // Windows SDK: <SDK>/bin/<version>/x64/dxc.exe (highest version first).
    std::string sdk_bin;
    const std::string sdk_dir = env_value("WindowsSdkDir");
    if (!sdk_dir.empty()) {
        sdk_bin = sdk_dir;
        if (!sdk_bin.empty() && sdk_bin.back() != '\\' && sdk_bin.back() != '/') {
            sdk_bin.push_back('\\');
        }
        sdk_bin += "bin\\";
    } else {
        sdk_bin = "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\";
    }

    std::vector<std::string> versions;
    WIN32_FIND_DATAA fd{};
    const std::string pattern = sdk_bin + "*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && fd.cFileName[0] != '.') {
                versions.push_back(fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(versions.begin(), versions.end(), std::greater<std::string>());
    for (const auto& v : versions) {
        out.push_back(sdk_bin + v + "\\x64\\dxc.exe");
    }

    // PATH.
    const std::string path_env = env_value("PATH");
    size_t start = 0;
    while (start <= path_env.size()) {
        const size_t sep = path_env.find(';', start);
        const std::string entry = path_env.substr(start, (sep == std::string::npos) ? std::string::npos : sep - start);
        if (!entry.empty()) {
            std::string p = entry;
            if (p.back() != '\\' && p.back() != '/') {
                p.push_back('\\');
            }
            out.push_back(p + "dxc.exe");
        }
        if (sep == std::string::npos) {
            break;
        }
        start = sep + 1;
    }
}

std::string first_line(const std::string& text) {
    const size_t nl = text.find_first_of("\r\n");
    return (nl == std::string::npos) ? text : text.substr(0, nl);
}

#endif // _WIN32

} // namespace

sc_dxil_backend_info sc_dxil_probe() {
    sc_dxil_backend_info info;
    info.name = "none";
#ifdef _WIN32
    std::vector<std::string> candidates;
    collect_candidates(candidates);
    for (const auto& candidate : candidates) {
        if (candidate.empty() || !is_file(candidate)) {
            continue;
        }
        std::string log;
        unsigned long code = 1;
        if (!run_capture(quote(candidate) + " --version", log, code)) {
            continue;
        }
        if (code != 0 || log.empty()) {
            continue;
        }
        info.available = true;
        info.name = "dxc-cli";
        info.path = candidate;
        info.version = first_line(log);
        info.note = "subprocess backend; dxc emits a DXBC container with a DXIL chunk";
        return info;
    }
    info.note = "dxc.exe not found (checked SC_DXC_PATH, DXC_PATH, Windows SDK bin, PATH)";
    return info;
#else
    info.note = "DXC probe implemented for Windows only";
    return info;
#endif
}

sc_dxc_cli_emitter::sc_dxc_cli_emitter(std::string dxc_path) : dxc_path_(std::move(dxc_path)) {}

sc_dxil_backend_info sc_dxc_cli_emitter::info() const {
    if (!cached_.available) {
        cached_ = sc_dxil_probe();
        if (cached_.available && !dxc_path_.empty() && cached_.path != dxc_path_) {
            cached_.path = dxc_path_;
        }
        if (!cached_.available) {
            cached_.name = "dxc-cli";
            cached_.path = dxc_path_;
        }
    }
    return cached_;
}

bool sc_dxc_cli_emitter::compile(const sc_dxil_request& request, sc_dxil_result& result) {
    result = sc_dxil_result{};
    result.backend = "dxc-cli";
#ifdef _WIN32
    const sc_dxil_backend_info backend = info();
    if (!backend.available || dxc_path_.empty()) {
        result.log = backend.note.empty() ? "dxc.exe unavailable" : backend.note;
        return false;
    }

    static unsigned counter = 0;
    const std::string base = temp_dir() + "sc_dxc_" + std::to_string(GetCurrentProcessId()) + "_" +
                             std::to_string(++counter);
    const std::string src_path = base + ".hlsl";
    const std::string out_path = base + ".dxil";

    {
        std::ofstream f(src_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            result.log = "could not write temporary HLSL to " + src_path;
            return false;
        }
        f.write(request.hlsl.data(), static_cast<std::streamsize>(request.hlsl.size()));
        f.flush();
        if (!f.good()) {
            result.log = "short write of temporary HLSL";
            return false;
        }
    }

    DeleteFileA(out_path.c_str());
    std::string cmd = quote(dxc_path_) + " -nologo -T " + request.profile + " -E " + request.entry + " -Fo " +
                      quote(out_path) + " " + quote(src_path);
    if (!request.extra_args.empty()) {
        cmd += " " + request.extra_args;
    }

    std::string log;
    unsigned long code = 1;
    const bool ran = run_capture(cmd, log, code);
    result.log = log;
    if (!ran) {
        result.log += "\nfailed to launch dxc.exe";
    }

    if (code == 0) {
        std::ifstream f(out_path, std::ios::binary | std::ios::ate);
        if (f) {
            const std::streamsize size = f.tellg();
            if (size > 0) {
                result.dxil.resize(static_cast<size_t>(size));
                f.seekg(0);
                f.read(reinterpret_cast<char*>(result.dxil.data()), size);
                result.ok = f.good() || f.eof();
            }
        }
        if (!result.ok) {
            result.log += "\ndxc reported success but produced no readable output blob";
        }
    }

    DeleteFileA(src_path.c_str());
    DeleteFileA(out_path.c_str());
    return result.ok;
#else
    (void)request;
    result.log = "dxc subprocess backend is Windows-only";
    return false;
#endif
}

sc_noop_emitter::sc_noop_emitter(std::string why) : why_(std::move(why)) {}

sc_dxil_backend_info sc_noop_emitter::info() const {
    sc_dxil_backend_info info;
    info.available = false;
    info.name = "noop";
    info.note = why_;
    return info;
}

bool sc_noop_emitter::compile(const sc_dxil_request& request, sc_dxil_result& result) {
    (void)request;
    result = sc_dxil_result{};
    result.ok = false;
    result.backend = "noop";
    result.log = why_;
    return false;
}

std::unique_ptr<sc_dxil_emitter> sc_make_dxil_emitter() {
    const sc_dxil_backend_info probe = sc_dxil_probe();
    if (probe.available) {
        return std::unique_ptr<sc_dxil_emitter>(new sc_dxc_cli_emitter(probe.path));
    }
    return std::unique_ptr<sc_dxil_emitter>(new sc_noop_emitter(probe.note));
}

} // namespace sc
