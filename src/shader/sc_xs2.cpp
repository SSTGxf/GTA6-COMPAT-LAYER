// ScarlettCompat shader pipeline - Phase 3: XS2 microcode discovery (MIT).
#include "shader/sc_xs2.h"

#include <algorithm>
#include <cstdio>

namespace sc {
namespace {

constexpr uint32_t kHeaderScanBytes = 64;

// A payload with fewer words than this (including the terminator) is not a
// shader body; keeps one stray constant from classifying a blob as microcode.
constexpr uint32_t kMinCodeWords = 8;

uint32_t rd32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void json_escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    out.push_back('"');
}

std::string json_string_array(const std::vector<std::string>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        json_escape(out, values[i]);
    }
    out.push_back(']');
    return out;
}

// Every 4-byte aligned s_endpgm word. Microcode is word-aligned, so an aligned
// scan is the right granularity.
void find_terminators(const uint8_t* payload, uint32_t payload_size, std::vector<uint32_t>& out) {
    for (uint32_t off = 0; off + 4 <= payload_size; off += 4) {
        if (rd32le(payload + off) == kXs2Endpgm) {
            out.push_back(off);
        }
    }
}

// Corroborated header candidates only: a dword that equals the payload size, or
// one that is 4-aligned and lands inside the code region before the terminator.
// Speculative "might be a version/count" guesses are not reported.
void probe_header(const uint8_t* payload, uint32_t payload_size, uint32_t first_terminator,
                  sc_xs2_header_probe& probe) {
    const uint32_t scan = std::min<uint32_t>(payload_size, kHeaderScanBytes);
    probe.scanned_bytes = scan;
    bool claimed_size = false;
    bool claimed_code = false;
    for (uint32_t off = 0; off + 4 <= scan; off += 4) {
        const uint32_t value = rd32le(payload + off);
        sc_xs2_field_candidate field;
        field.offset = off;
        field.value = value;

        if (!claimed_size && value == payload_size && value >= 32) {
            field.interpretation = "payload_size";
            claimed_size = true;
        } else if (!claimed_code && value != 0 && (value % 4u) == 0 && value >= 16 && value < first_terminator &&
                   value + 4 <= payload_size) {
            field.interpretation = "code_offset_candidate";
            claimed_code = true;
        } else {
            continue;
        }
        probe.fields.push_back(field);
    }
}

} // namespace

std::string sc_xs2_header_probe::to_json() const {
    std::string s = "{\"scanned_bytes\":" + std::to_string(scanned_bytes) + ",\"fields\":[";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) {
            s.push_back(',');
        }
        const sc_xs2_field_candidate& f = fields[i];
        s += "{\"offset\":" + std::to_string(f.offset);
        s += ",\"value\":" + std::to_string(f.value);
        s += ",\"interpretation\":";
        json_escape(s, f.interpretation);
        s.push_back('}');
    }
    s += "]}";
    return s;
}

std::string sc_xs2_code_region::to_json() const {
    return "{\"begin\":" + std::to_string(begin) + ",\"end\":" + std::to_string(end) +
           ",\"terminator\":" + std::to_string(terminator_offset) +
           ",\"word_count\":" + std::to_string((end - begin) / 4) + "}";
}

std::string sc_xs2_discovery::to_json() const {
    std::string s = "{";
    s += "\"found\":";
    s += found ? "true" : "false";
    s += ",\"code_start_corroborated\":";
    s += code_start_corroborated ? "true" : "false";
    s += ",\"envelope\":";
    json_escape(s, envelope);
    s += ",\"payload_offset\":" + std::to_string(payload_offset);
    s += ",\"payload_size\":" + std::to_string(payload_size);
    s += ",\"terminators\":[";
    for (size_t i = 0; i < terminator_offsets.size(); ++i) {
        if (i != 0) {
            s.push_back(',');
        }
        s += std::to_string(terminator_offsets[i]);
    }
    s += "],\"code_regions\":[";
    for (size_t i = 0; i < code_regions.size(); ++i) {
        if (i != 0) {
            s.push_back(',');
        }
        s += code_regions[i].to_json();
    }
    s += "],\"header\":" + header.to_json();
    s += ",\"evidence\":" + json_string_array(evidence);
    s += ",\"open_questions\":" + json_string_array(open_questions);
    s.push_back('}');
    return s;
}

sc_xs2_discovery sc_xs2_discover(const void* data, size_t size, uint32_t payload_offset, uint32_t payload_size) {
    sc_xs2_discovery d;
    if (data == nullptr || size == 0) {
        d.open_questions.push_back("empty blob");
        return d;
    }
    if (size > 0xffffffffull) {
        d.open_questions.push_back("blob larger than 4 GiB; this path only handles 32-bit sizes");
        return d;
    }
    if (payload_size == 0) {
        payload_offset = 0;
        payload_size = static_cast<uint32_t>(size);
        d.envelope = "raw";
    }
    if (payload_offset > size || static_cast<uint64_t>(payload_offset) + payload_size > size) {
        d.open_questions.push_back("payload range lies outside the blob");
        return d;
    }

    d.payload_offset = payload_offset;
    d.payload_size = payload_size;
    const uint8_t* payload = static_cast<const uint8_t*>(data) + payload_offset;

    d.evidence.push_back("observed: payload " + std::to_string(payload_size) + " bytes at offset " +
                         std::to_string(payload_offset));
    if (payload_size % 4 != 0) {
        d.evidence.push_back("observed: payload size is not a multiple of 4; microcode is word-aligned");
        d.open_questions.push_back("unaligned payload: a wrapper or a trailing field must be stripped first");
        return d;
    }

    std::vector<uint32_t> terminators;
    find_terminators(payload, payload_size, terminators);
    for (uint32_t off : terminators) {
        d.terminator_offsets.push_back(payload_offset + off);
    }
    if (terminators.empty()) {
        d.evidence.push_back("observed: no s_endpgm word (0xBF810000) anywhere in the payload");
        d.open_questions.push_back(
            "if the corpus shows XS2 payloads without s_endpgm, the terminator assumption is wrong for this title");
        return d;
    }

    const uint32_t first_terminator = terminators.front();
    d.evidence.push_back("observed: " + std::to_string(terminators.size()) + " s_endpgm word(s) at 0x" +
                         std::to_string(payload_offset + first_terminator));
    if (first_terminator / 4 + 1 < kMinCodeWords) {
        d.evidence.push_back("observed: first terminator after only " + std::to_string(first_terminator / 4) +
                             " word(s); below the " + std::to_string(kMinCodeWords) + "-word floor");
        return d;
    }

    probe_header(payload, payload_size, first_terminator, d.header);
    for (const auto& f : d.header.fields) {
        d.evidence.push_back("inferred: header field @" + std::to_string(f.offset) + " = " + std::to_string(f.value) +
                             " looks like " + f.interpretation);
    }

    uint32_t code_begin = 0;
    for (const auto& f : d.header.fields) {
        if (f.interpretation == "code_offset_candidate") {
            code_begin = f.value;
            d.code_start_corroborated = true;
            break;
        }
    }
    d.evidence.push_back(d.code_start_corroborated
                             ? "inferred: code region starts at the code_offset field value " + std::to_string(code_begin)
                             : "inferred: code region starts at the payload start (no code_offset field corroborated)");

    for (uint32_t terminator : terminators) {
        sc_xs2_code_region region;
        region.begin = payload_offset + code_begin;
        region.terminator_offset = payload_offset + terminator;
        if (region.terminator_offset + 4 <= region.begin) {
            continue;
        }
        region.end = region.terminator_offset + 4;
        d.code_regions.push_back(region);
    }
    if (d.code_regions.empty()) {
        d.open_questions.push_back("terminators precede the inferred code start; the header field guess is inconsistent");
        return d;
    }

    d.found = true;
    d.open_questions.push_back(
        "instruction stream is not decoded: RDNA2 encoding tables are the next phase, so register and wave64 "
        "semantics are unverified for this payload");
    d.open_questions.push_back(
        "header field offsets are inferred from self-consistency, not from a documented XS2 layout");
    if (!d.code_start_corroborated) {
        d.open_questions.push_back("no header dword pointed at the code start; region bounds may include a header");
    }
    return d;
}

} // namespace sc
