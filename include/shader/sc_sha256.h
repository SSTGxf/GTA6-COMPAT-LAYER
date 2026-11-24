// ScarlettCompat shader pipeline - content hashing.
// MIT. Clean-room implementation of the public SHA-256 and FNV-1a algorithms.
//
// This module is part of the parallel shader subsystem and is not wired into
// the boot path. See docs/SHADER-PIPELINE-PHASE0.md.
#ifndef SC_SHADER_SHA256_H
#define SC_SHADER_SHA256_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace sc {

struct sha256_digest {
    uint8_t bytes[32];
};

// Standard SHA-256. Safe with (data == nullptr, size == 0).
sha256_digest sc_sha256(const void* data, size_t size);

// Lower-case hex of arbitrary bytes.
std::string sc_hex(const void* data, size_t size);
std::string sc_hex(const sha256_digest& d);

// Convenience: hex SHA-256 of a buffer.
std::string sc_sha256_hex(const void* data, size_t size);

// FNV-1a 64-bit (fast, non-cryptographic; used for capture de-duplication).
uint64_t sc_fnv1a64(const void* data, size_t size, uint64_t seed = 0xcbf29ce484222325ull);

// Incremental FNV-1a, handy when a signature spans many fields.
class sc_fnv1a64_stream {
public:
    void add(const void* data, size_t size);
    void add_u32(uint32_t v);
    void add_u64(uint64_t v);
    uint64_t value() const { return value_; }

private:
    uint64_t value_ = 0xcbf29ce484222325ull;
};

} // namespace sc

#endif // SC_SHADER_SHA256_H
