// ScarlettCompat shader pipeline - Phase 3: RDNA2 (XS2) microcode discovery.
// MIT. New file; not wired into the boot path.
//
// WHAT THIS IS
//   The discovery/parse path for shader payloads that are NOT a DXIL container,
//   i.e. the "precompiled RDNA2 XS2" case described in docs/ARCHITECTURE.md.
//   It does not decode or lift instructions -- it answers, from bytes only:
//     * is this a microcode payload at all?
//     * where does the code region start and end?
//     * which header words look like a size / a code offset, and at what offsets?
//   That is what is needed to triage the first real corpus dump.
//
// OBSERVED vs INFERRED
//   OBSERVED: a region ends on one of the documented SOPP terminators --
//   s_endpgm (0xBF810000), s_endpgm_saved (0xBF9B0000) or
//   s_endpgm_ordered_ps_done (0xBF9E0000). The table lives in sc_isa.h so there
//   is one copy; s_code_end is a marker, not a terminator. Presence and offsets
//   are read from the blob. Evidence lines
//   that start with "observed:" report only such facts.
//   INFERRED: no real Scarlett shader has been seen, so every header field is a
//   candidate found by self-consistency (a dword equal to the payload size, a
//   dword that is 4-aligned and lands inside the code region), not a fixed
//   offset from a specification. Only corroborated candidates are reported, and
//   their evidence lines start with "inferred:".
//
//   The instruction stream is deliberately NOT walked: that needs the RDNA2
//   encoding tables, which is the next phase. Consequences are listed in
//   `open_questions` rather than guessed at.
#ifndef SC_SHADER_XS2_H
#define SC_SHADER_XS2_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sc {

// Documented AMD ISA constant: `s_endpgm` is encoded as 0xBF810000 on
// GCN/RDNA. Used only as a terminator marker.
constexpr uint32_t kXs2Endpgm = 0xBF810000u;

struct sc_xs2_field_candidate {
    uint32_t offset = 0; // byte offset inside the payload
    uint32_t value = 0;
    std::string interpretation; // "payload_size" | "code_offset_candidate"
};

struct sc_xs2_header_probe {
    uint32_t scanned_bytes = 0;
    std::vector<sc_xs2_field_candidate> fields; // corroborated candidates only
    std::string to_json() const;
};

struct sc_xs2_code_region {
    uint32_t begin = 0;             // absolute offset of the first code word
    uint32_t end = 0;               // one past the terminating s_endpgm
    uint32_t terminator_offset = 0; // absolute offset of the s_endpgm word
    std::string to_json() const;
};

struct sc_xs2_discovery {
    bool found = false;
    // True when a header field points at the code start, false when the region
    // was assumed to start at the payload start. This is the one corroboration
    // signal that changes how the region bounds should be read.
    bool code_start_corroborated = false;
    std::string envelope = "raw"; // "raw" or "dxbc-chunk:<name>"
    uint32_t payload_offset = 0;
    uint32_t payload_size = 0;
    std::vector<uint32_t> terminator_offsets; // absolute offsets of s_endpgm words
    std::vector<sc_xs2_code_region> code_regions;
    sc_xs2_header_probe header;
    std::vector<std::string> evidence;      // each line starts with observed:/inferred:
    std::vector<std::string> open_questions;
    std::string to_json() const;
};

// Probes a payload. Pass (payload_offset, payload_size) to probe a region of a
// larger blob (e.g. a chunk payload); pass 0/0 to probe the whole blob.
sc_xs2_discovery sc_xs2_discover(const void* data, size_t size, uint32_t payload_offset = 0,
                                 uint32_t payload_size = 0);

} // namespace sc

#endif // SC_SHADER_XS2_H
