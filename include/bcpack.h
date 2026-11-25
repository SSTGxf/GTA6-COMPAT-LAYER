// bcpack.h — BCPack milestone B1: the BC1 entropy layer + block assembler.
//
// B1 SCOPE ONLY (docs/FREEBUFF-BCPACK-TEXTURE-SPEC.md Part 1):
//   * the 12-lane 16-bit rANS entropy layer and the BC1 block rebuilder,
//   * our own spec-built encoder exists only in the test harness,
//   * no BC2..BC7, no BC6H history calibration, no DirectStorage wiring, no
//     game data, no GPU.
//
// The format is implemented from the published reconstruction in
// docs/BCPACK-B1.md (record layout, rANS, generated probability models,
// scheduler). Nothing here is copied from another BCPack implementation: the
// learned BC1 tables are OUR tables (see ScBc1Tables) and the module never
// reads a shipped table file.
//
// Decoding fails closed: on any error the destination buffer is untouched and
// the status says why. Callers must treat a non-OK status as "no output".
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- chunk-table storage field ------------------------------------------
//
// .ttpf chunk-table entry bits 26..29 record how the NEXT chunk is stored:
// 0 raw, 1..7 BCPack BC1..BC7, 8 zlib (ttpf-spec-reference/SPEC.md, "agreed"
// in both sources). This is DETECTION ONLY for the later DirectStorage
// wiring: nothing in this layer decompresses, wires or dispatches on it yet.
enum ScBcpackStorage {
    SC_BCPACK_STORAGE_RAW = 0,
    SC_BCPACK_STORAGE_BC1 = 1,
    SC_BCPACK_STORAGE_BC2 = 2,
    SC_BCPACK_STORAGE_BC3 = 3,
    SC_BCPACK_STORAGE_BC4 = 4,
    SC_BCPACK_STORAGE_BC5 = 5,
    SC_BCPACK_STORAGE_BC6H = 6,
    SC_BCPACK_STORAGE_BC7 = 7,
    SC_BCPACK_STORAGE_ZLIB = 8,
    SC_BCPACK_STORAGE_UNKNOWN = 9,  // 9..15: no meaning in either source
};

uint32_t WINAPI ScBcpackChunkStorage(uint64_t entry);
const char* WINAPI ScBcpackChunkStorageName(uint32_t storage);

// ---- BC1 -----------------------------------------------------------------

#define SC_BCPACK_BC1_LANES 12
#define SC_BCPACK_BC1_UNIT_BYTES 8   // one BC1 block per coding unit
#define SC_BC1_HISTORY 255           // virtual blocks -1 .. -255

// The BC1 tables the decoder uses. They are learned constants (not part of the
// bitstream): history[k] is virtual block -(k+1) as {r0,g0,b0,r1,g1,b1} with
// r/b in 0..31 and g in 0..63; quadrant[s] is the quadrant code of selector
// symbol s, a permutation of 0..255.
//
// B1 ships self-derived tables (all-zero history, identity quadrant mapping)
// so the harness can prove the learn-tables path reproduces exactly what the
// decoder uses. They are NOT the reference's fitted tables and therefore do
// not decode real game chunks; see docs/BCPACK-B1.md.
typedef struct ScBc1Tables {
    uint8_t history[SC_BC1_HISTORY][6];
    uint8_t quadrant[256];
} ScBc1Tables;

void WINAPI ScBc1TablesDefault(ScBc1Tables* tables);

enum ScBcpackStatus {
    SC_BCPACK_OK = 0,
    SC_BCPACK_BAD_PARAMS = 1,        // null pointer / null tables
    SC_BCPACK_BAD_SIZE = 2,          // outBytes 0 or not a multiple of 8
    SC_BCPACK_TRUNCATED = 3,         // stream ended while a byte was needed
    SC_BCPACK_BAD_HEADER = 4,        // impossible mode / width reduction
    SC_BCPACK_BAD_SYMBOL = 5,        // a lane symbol is not valid for BC1
    SC_BCPACK_BAD_END = 6,           // record ended with state != L
    SC_BCPACK_TRAILING = 7,          // chunk bytes left after every lane ended
    SC_BCPACK_ROUND_LIMIT = 8,       // scheduler made no progress
    SC_BCPACK_NO_OUTPUT = 9,         // internal: assembly needed more symbols
};

const char* WINAPI ScBcpackStatusName(uint32_t status);

typedef struct ScBcpackDiag {
    uint32_t status;
    uint32_t rounds;
    uint32_t bytesConsumed;
    uint32_t units;        // completed coding units (BC1 blocks)
    uint32_t symbols[SC_BCPACK_BC1_LANES];
} ScBcpackDiag;

// Decode one BC1 chunk: srcBytes of packed chunk, outBytes of standard BC1
// blocks in chunk (memory) order. dst is written only when the call returns
// SC_BCPACK_OK; on failure it is left untouched. diag may be null.
uint32_t WINAPI ScBcpackDecodeBc1(const void* src, uint32_t srcBytes,
                                  uint32_t outBytes,
                                  const ScBc1Tables* tables, void* dst,
                                  ScBcpackDiag* diag);

// ---- model tables (shared with the harness) ------------------------------

// Fills cum[0..n] for model (q, width): cum[0] = 1 is the FIN slot boundary
// and cum[n] = 65536. Returns the symbol count n = 2^width, or 0 when q/width
// are out of range or cap is too small (needs n+1 entries).
uint32_t WINAPI ScBcpackModelCum(uint32_t q, uint32_t width, uint32_t* cum,
                                 uint32_t cap);

// ---- harness support -----------------------------------------------------

// Interleave one record per lane into a chunk (the encoder side of the
// scheduler). laneBytes[l] is lane l's record (header + state + payload);
// streamCap must hold the whole stream. On success *streamLen is the chunk
// size and the stream decodes with ScBcpackDecodeBc1 for the same outBytes
// when the records carry the symbols the decoder expects. Returns a status.
uint32_t WINAPI ScBcpackBuildStream(const uint8_t* const laneBytes[SC_BCPACK_BC1_LANES],
                                    const uint32_t laneLens[SC_BCPACK_BC1_LANES],
                                    uint32_t outBytes, void* stream,
                                    uint32_t streamCap, uint32_t* streamLen,
                                    ScBcpackDiag* diag);

#ifdef __cplusplus
}
#endif
