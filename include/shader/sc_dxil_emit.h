// ScarlettCompat shader pipeline - DXIL emission seam (MIT, new file).
//
// The translation step "HLSL -> DXIL" is deliberately behind this interface so
// it can be swapped without touching capture or the layer:
//   * sc_dxc_cli_emitter  : drives dxc.exe (verified present in Windows SDK
//                           10.0.26100.0 on this machine) in a subprocess.
//   * sc_noop_emitter     : honest "not available" backend so callers never
//                           have to special-case a missing DXC.
// A future in-process backend (dxcompiler.dll / IDxcCompiler3) only has to
// implement this interface; nothing else changes.
#ifndef SC_SHADER_DXIL_EMIT_H
#define SC_SHADER_DXIL_EMIT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sc {

struct sc_dxil_request {
    std::string hlsl;              // source text
    std::string entry = "main";    // entry point
    std::string profile = "vs_6_6";// target profile (SM 6.6/6.7 for RDNA1)
    std::string extra_args;        // appended verbatim to the dxc command line
};

struct sc_dxil_result {
    bool ok = false;
    std::vector<uint8_t> dxil; // container bytes (dxc emits a DXBC envelope)
    std::string log;           // compiler output (never contains secrets)
    std::string backend;
};

struct sc_dxil_backend_info {
    bool available = false;
    std::string name;
    std::string path;
    std::string version;
    std::string note;
};

class sc_dxil_emitter {
public:
    virtual ~sc_dxil_emitter() = default;
    virtual sc_dxil_backend_info info() const = 0;
    virtual bool compile(const sc_dxil_request& request, sc_dxil_result& result) = 0;
};

// Locates dxc.exe (SC_DXC_PATH, DXC_PATH, Windows SDK, PATH) and verifies it by
// running --version.
sc_dxil_backend_info sc_dxil_probe();

// Best backend for this machine: dxc when found, otherwise the noop backend.
std::unique_ptr<sc_dxil_emitter> sc_make_dxil_emitter();

class sc_dxc_cli_emitter final : public sc_dxil_emitter {
public:
    explicit sc_dxc_cli_emitter(std::string dxc_path);
    sc_dxil_backend_info info() const override;
    bool compile(const sc_dxil_request& request, sc_dxil_result& result) override;

private:
    std::string dxc_path_;
    mutable sc_dxil_backend_info cached_;
};

class sc_noop_emitter final : public sc_dxil_emitter {
public:
    explicit sc_noop_emitter(std::string why);
    sc_dxil_backend_info info() const override;
    bool compile(const sc_dxil_request& request, sc_dxil_result& result) override;

private:
    std::string why_;
};

} // namespace sc

#endif // SC_SHADER_DXIL_EMIT_H
