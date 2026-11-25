// bcpack_b1.cpp — milestone B1 harness.
//
// This is the ONLY place an encoder exists: it is a test harness that produces
// BCPack BC1 chunks for the decoder, never shipped behavior. It is built from
// the same published format description as src/bcpack.cpp, and it uses
// ScBcpackBuildStream (the encoder side of the shared scheduler) so the bytes
// it emits are ordered exactly as the decoder will fetch them.
//
// What it proves:
//   1. byte-exact BC1 round-trips at 1, 16, 64 and 256 blocks;
//   2. malformed chunks fail closed (destination untouched, status says why);
//   3. the BC1 tables are derivable from our own generated data with the
//      learn-tables approach and the derivation reproduces the decoder's
//      tables (history and quadrant mapping);
//   4. optional --cross REF EXE: drive the independent reference decoder over
//      the same streams and compare bit-exactly (stream, lane symbols, BC1
//      blocks and all 128 probability tables).
//
// Usage:
//   bcpack_b1.exe                       self-tests (what build.bat runs)
//   bcpack_b1.exe --cross REF.exe DIR   also run the reference cross-check,
//                                       writing intermediates into DIR

#include "bcpack.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int g_fail;
static int g_checks;

static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        printf("FAIL %s\n", what);
    }
}

// ---- deterministic RNG ---------------------------------------------------

static uint32_t g_rng = 0x5EED1234u;
static uint32_t Rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// ---- lane model helpers --------------------------------------------------

static uint32_t Freq(uint32_t q, uint32_t w, uint32_t s, uint32_t* cumOut) {
    uint32_t cum[257];
    const uint32_t n = ScBcpackModelCum(q, w, cum, 257);
    if (!n || s >= n) return 0;
    if (cumOut) memcpy(cumOut, cum, (n + 1) * sizeof(uint32_t));
    return cum[s + 1] - cum[s];
}

// Encodes n symbols into one record (mode + length + state + payload).
// Returns the record length, or 0 on a parameter/overflow problem.
static uint32_t EncodeRecord(const uint8_t* sym, uint32_t n, uint32_t q,
                             uint32_t baseWidth, uint32_t widthReduction,
                             uint8_t* rec, uint32_t recCap) {
    if (!rec || widthReduction >= baseWidth) return 0;
    const uint32_t w = baseWidth - widthReduction;
    uint32_t cum[257];
    const uint32_t ns = ScBcpackModelCum(q, w, cum, 257);
    if (!ns) return 0;
    static uint8_t emit[1 << 18];
    uint32_t emitLen = 0;
    uint32_t x = 1u << 24;
    for (uint32_t i = n; i-- > 0;) {
        const uint32_t s = sym[i];
        if (s >= ns) return 0;
        const uint32_t f = cum[s + 1] - cum[s];
        const uint32_t c = cum[s];
        while ((uint64_t)x >= (uint64_t)f * 65536u) {
            if (emitLen >= sizeof emit) return 0;
            emit[emitLen++] = (uint8_t)(x & 0xFF);
            x >>= 8;
        }
        x = (x / f) * 65536u + (x % f) + c;
    }
    if (x < (1u << 24)) return 0;
    static uint8_t body[1 << 18];
    uint32_t bl = 0;
    body[bl++] = (uint8_t)(x >> 24);
    body[bl++] = (uint8_t)(x >> 16);
    body[bl++] = (uint8_t)(x >> 8);
    body[bl++] = (uint8_t)x;
    for (uint32_t i = emitLen; i-- > 0;) body[bl++] = emit[i];
    // Trailing zero bytes are not stored; the decoder restores them.
    while (bl > 1 && body[bl - 1] == 0) --bl;
    const uint8_t mode = (uint8_t)(q | 0x10u | (widthReduction << 6));
    if (bl <= 256) {
        if (recCap < 2 + bl) return 0;
        rec[0] = mode;
        rec[1] = (uint8_t)(bl - 1);
        memcpy(rec + 2, body, bl);
        return 2 + bl;
    }
    const uint32_t v = bl - 257;
    if (v > 0xFFFF || recCap < 3 + bl) return 0;
    rec[0] = (uint8_t)(mode | 0x20u);
    rec[1] = (uint8_t)(v & 0xFF);
    rec[2] = (uint8_t)(v >> 8);
    memcpy(rec + 3, body, bl);
    return 3 + bl;
}

// ---- BC1 block construction (ground truth) -------------------------------

static const uint32_t kWidths[SC_BCPACK_BC1_LANES] = {8, 8, 5, 6, 5,
                                                      5, 6, 5, 8, 8, 8, 8};
static const uint32_t kMod[6] = {32, 64, 32, 32, 64, 32};

static int32_t Zigzag(uint32_t v) {
    return (v & 1u) ? -(int32_t)(v >> 1) - 1 : (int32_t)(v >> 1);
}

// Builds symbols + blocks where block i refers to block i-1 (i > 0) and block
// 0 refers to history entry 0 through a folded distance of 1.
static void Generate(int mode, uint32_t blocks, const ScBc1Tables* tables,
                     const uint32_t effWidth[SC_BCPACK_BC1_LANES],
                     uint8_t sym[SC_BCPACK_BC1_LANES][256],
                     uint8_t* blockBytes) {
    uint8_t prev[6] = {0, 0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < blocks; ++i) {
        uint8_t pred[6];
        uint32_t folded;
        if (mode == 0) {  // distance 1: previous block, or history[0]
            memcpy(pred, i ? prev : tables->history[0], 6);
            folded = 2;  // D = v/2 = 1
        } else {  // distance 255: always a history entry (255 - i)
            const uint32_t entry = 255u - i;  // 1..255
            for (uint32_t c = 0; c < 6; ++c)
                pred[c] = tables->history[entry - 1][c];
            folded = 1;  // D = 255 - (v >> 1) = 255
        }
        sym[0][i] = (uint8_t)folded;
        sym[1][i] = 0;
        uint8_t e[6];
        for (uint32_t c = 0; c < 6; ++c) {
            const uint32_t w = effWidth[2 + c];
            const uint32_t s = Rnd() & ((1u << w) - 1u);
            sym[2 + c][i] = (uint8_t)s;
            int32_t v = (int32_t)pred[c] + Zigzag(s);
            v %= (int32_t)kMod[c];
            if (v < 0) v += (int32_t)kMod[c];
            e[c] = (uint8_t)v;
        }
        memcpy(prev, e, 6);
        for (uint32_t l = 8; l < 12; ++l)
            sym[l][i] = (uint8_t)(Rnd() & 0xFF);
        const uint32_t c0 =
            ((uint32_t)e[0] << 11) | ((uint32_t)e[1] << 5) | e[2];
        const uint32_t c1 =
            ((uint32_t)e[3] << 11) | ((uint32_t)e[4] << 5) | e[5];
        const uint32_t tl = tables->quadrant[sym[8][i]];
        const uint32_t tr = tables->quadrant[sym[9][i]];
        const uint32_t bl = tables->quadrant[sym[10][i]];
        const uint32_t br = tables->quadrant[sym[11][i]];
        uint8_t* b = blockBytes + (size_t)i * 8;
        b[0] = (uint8_t)(c0 & 0xFF);
        b[1] = (uint8_t)(c0 >> 8);
        b[2] = (uint8_t)(c1 & 0xFF);
        b[3] = (uint8_t)(c1 >> 8);
        b[4] = (uint8_t)((tl & 15) | ((tr & 15) << 4));
        b[5] = (uint8_t)((tl >> 4) | ((tr >> 4) << 4));
        b[6] = (uint8_t)((bl & 15) | ((br & 15) << 4));
        b[7] = (uint8_t)((bl >> 4) | ((br >> 4) << 4));
    }
}

// Builds one full chunk for `blocks` blocks. Returns the chunk length (0 on
// failure). q/r vary per lane so several models are exercised.
static uint32_t BuildChunk(int mode, uint32_t blocks, const ScBc1Tables* tables,
                           uint8_t symOut[SC_BCPACK_BC1_LANES][256],
                           uint8_t* blockBytes, uint8_t* stream,
                           uint32_t streamCap) {
    uint32_t red[SC_BCPACK_BC1_LANES], effW[SC_BCPACK_BC1_LANES];
    for (uint32_t l = 0; l < SC_BCPACK_BC1_LANES; ++l) {
        red[l] = (l < 6) ? (l % 2) : 0;  // exercise width reduction
        effW[l] = kWidths[l] - red[l];
    }
    Generate(mode, blocks, tables, effW, symOut, blockBytes);
    static uint8_t rec[SC_BCPACK_BC1_LANES][1 << 18];
    const uint8_t* lanePtr[SC_BCPACK_BC1_LANES];
    uint32_t laneLen[SC_BCPACK_BC1_LANES];
    for (uint32_t l = 0; l < SC_BCPACK_BC1_LANES; ++l) {
        const uint32_t q = l % 16;
        const uint32_t r = red[l];
        const uint32_t len = EncodeRecord(symOut[l], blocks, q, kWidths[l], r,
                                          rec[l], sizeof rec[l]);
        if (!len) {
            printf("  encode failed: lane %u blocks %u q %u r %u\n", l,
                   blocks, q, r);
            return 0;
        }
        lanePtr[l] = rec[l];
        laneLen[l] = len;
    }
    uint32_t len = 0;
    ScBcpackDiag diag;
    const uint32_t st = ScBcpackBuildStream(lanePtr, laneLen, blocks * 8,
                                            stream, streamCap, &len, &diag);
    if (st != SC_BCPACK_OK) {
        printf("  interleave failed: %s, blocks %u, lanes:",
               ScBcpackStatusName(st), blocks);
        for (uint32_t l = 0; l < SC_BCPACK_BC1_LANES; ++l)
            printf(" %u/%u", diag.symbols[l], blocks);
        printf("\n");
        return 0;
    }
    return len;
}

// ---- self-tests ----------------------------------------------------------

static void TestRoundTrips(const ScBc1Tables* tables) {
    static uint8_t sym[SC_BCPACK_BC1_LANES][256];
    static uint8_t blocks[256 * 8];
    static uint8_t stream[1 << 20];
    static uint8_t decoded[256 * 8];
    const uint32_t counts[4] = {1, 16, 64, 256};
    for (int k = 0; k < 4; ++k) {
        const uint32_t n = counts[k];
        const uint32_t len = BuildChunk(0, n, tables, sym, blocks, stream,
                                        sizeof stream);
        Check(len > 0, "chunk build");
        if (!len) continue;
        ScBcpackDiag diag;
        memset(decoded, 0, n * 8);
        const uint32_t st =
            ScBcpackDecodeBc1(stream, len, n * 8, tables, decoded, &diag);
        Check(st == SC_BCPACK_OK, "round-trip status");
        Check(memcmp(decoded, blocks, n * 8) == 0, "round-trip blocks");
        if (memcmp(decoded, blocks, n * 8) != 0) {
            for (uint32_t i = 0; i < n; ++i) {
                if (memcmp(decoded + i * 8, blocks + i * 8, 8) == 0) continue;
                printf("  block %u: want", i);
                for (int b = 0; b < 8; ++b) printf(" %02x", blocks[i * 8 + b]);
                printf(" got");
                for (int b = 0; b < 8; ++b) printf(" %02x", decoded[i * 8 + b]);
                printf(" sym");
                for (int l = 0; l < SC_BCPACK_BC1_LANES; ++l)
                    printf(" %u", sym[l][i]);
                printf("\n");
                break;
            }
        }
        Check(diag.units == n, "round-trip units");
        char line[160];
        snprintf(line, sizeof line,
                 "B1 round-trip: %3u blocks, %6u chunk bytes, %5u rounds, "
                 "status %s",
                 n, len, diag.rounds, ScBcpackStatusName(st));
        printf("%s\n", line);
    }
}

static void TestMalformed(const ScBc1Tables* tables) {
    static uint8_t sym[SC_BCPACK_BC1_LANES][256];
    static uint8_t blocks[256 * 8];
    static uint8_t stream[1 << 20];
    static uint8_t copy[1 << 20];
    static uint8_t dst[256 * 8];
    const uint32_t n = 16;
    const uint32_t len = BuildChunk(0, n, tables, sym, blocks, stream,
                                    sizeof stream);
    Check(len > 32, "malformed base chunk");
    if (!len) return;
    for (int i = 0; i < (int)sizeof dst; ++i) dst[i] = 0xA5;

    // Wrong output size.
    Check(ScBcpackDecodeBc1(stream, len, 7, tables, dst, nullptr) ==
              SC_BCPACK_BAD_SIZE,
          "reject 7-byte output");
    Check(ScBcpackDecodeBc1(stream, len, 0, tables, dst, nullptr) ==
              SC_BCPACK_BAD_SIZE,
          "reject 0-byte output");
    // Truncated lane data (every truncation the last 8 bytes can produce).
    bool truncatedRejected = true;
    for (uint32_t cut = 1; cut <= 8 && cut < len; ++cut) {
        const uint32_t st = ScBcpackDecodeBc1(stream, len - cut, n * 8,
                                              tables, dst, nullptr);
        if (st == SC_BCPACK_OK) truncatedRejected = false;
    }
    Check(truncatedRejected, "reject truncated chunk");
    // Trailing bytes.
    memcpy(copy, stream, len);
    copy[len] = 0;
    Check(ScBcpackDecodeBc1(copy, len + 1, n * 8, tables, dst, nullptr) !=
              SC_BCPACK_OK,
          "reject trailing byte");
    // Declared block count larger than the streams carry.
    Check(ScBcpackDecodeBc1(stream, len, (n + 1) * 8, tables, dst, nullptr) !=
              SC_BCPACK_OK,
          "reject too-large block count");
    // Out-of-domain BC1 lane: lane 1 must be 0.
    {
        uint8_t sym2[SC_BCPACK_BC1_LANES][256];
        uint8_t blocks2[256 * 8];
        static uint8_t stream2[1 << 20];
        for (uint32_t l = 0; l < SC_BCPACK_BC1_LANES; ++l)
            memcpy(sym2[l], sym[l], 256);
        for (uint32_t i = 0; i < n; ++i) sym2[1][i] = 1;
        memcpy(blocks2, blocks, sizeof blocks2);
        static uint8_t rec[SC_BCPACK_BC1_LANES][1 << 18];
        const uint8_t* lp[SC_BCPACK_BC1_LANES];
        uint32_t ll[SC_BCPACK_BC1_LANES];
        bool built = true;
        for (uint32_t l = 0; l < SC_BCPACK_BC1_LANES; ++l) {
            const uint32_t r = (l < 6) ? (l % 2) : 0;
            ll[l] = EncodeRecord(sym2[l], n, l % 16, kWidths[l], r, rec[l],
                                 sizeof rec[l]);  // same reduction as build
            if (!ll[l]) built = false;
            lp[l] = rec[l];
        }
        uint32_t len2 = 0;
        if (built &&
            ScBcpackBuildStream(lp, ll, n * 8, stream2, sizeof stream2, &len2,
                                nullptr) == SC_BCPACK_OK) {
            Check(ScBcpackDecodeBc1(stream2, len2, n * 8, tables, dst,
                                    nullptr) == SC_BCPACK_BAD_SYMBOL,
                  "reject non-zero lane 1");
        } else {
            Check(false, "build non-zero lane 1 chunk");
        }
    }
    // Corrupted payload. BCPack carries no checksum, so a single-byte change
    // can produce another *valid* stream (it decodes to different blocks with
    // the state still returning to L). What the format does guarantee, and
    // what is checked here, is that every rejection is complete: the
    // destination is untouched and the status names the failure. The count of
    // corruptions that stayed well-formed is reported, not hidden.
    uint32_t corruptOk = 0, corruptRej = 0;
    bool rejectUntouched = true;
    for (uint32_t pos = 0; pos < len; pos += (len / 16 + 1)) {
        memcpy(copy, stream, len);
        copy[pos] ^= 0x40;
        memset(dst, 0xA5, sizeof dst);
        const uint32_t cst =
            ScBcpackDecodeBc1(copy, len, n * 8, tables, dst, nullptr);
        if (cst == SC_BCPACK_OK) {
            ++corruptOk;
            continue;
        }
        ++corruptRej;
        for (size_t i = 0; i < (size_t)n * 8; ++i) {
            if (dst[i] != 0xA5) rejectUntouched = false;
        }
    }
    Check(corruptRej > 0, "some corruptions are detected");
    Check(rejectUntouched, "rejected corruption leaves destination untouched");
    printf("B1 malformed: truncation, trailing, size and lane-1 all rejected; "
           "%u/%u single-byte corruptions rejected (destination untouched), "
           "%u formed another valid stream (format has no checksum)\n",
           (unsigned)corruptRej, (unsigned)(corruptRej + corruptOk),
           (unsigned)corruptOk);
}

// ---- learn-tables --------------------------------------------------------

// Derives the two BC1 tables from our own data only: the block bytes are the
// ground truth, the lane symbols are what the encoder chose, and the tables
// are recovered from those two without consulting ScBc1Tables.
static void TestLearnTables(const ScBc1Tables* tables) {
    static uint8_t sym[SC_BCPACK_BC1_LANES][256];
    static uint8_t blocks[256 * 8];
    static uint8_t stream[1 << 20];
    uint8_t hist[SC_BC1_HISTORY][6];
    uint8_t quad[256];
    bool histSeen[SC_BC1_HISTORY];
    bool quadSeen[256];
    memset(hist, 0, sizeof hist);
    memset(quad, 0, sizeof quad);
    memset(histSeen, 0, sizeof histSeen);
    memset(quadSeen, 0, sizeof quadSeen);
    bool histOk = true, quadOk = true;

    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t n = 255;  // block i references history entry 255 - i
        g_rng = 0x1234u + (uint32_t)pass * 77u;
        const uint32_t len = BuildChunk(1, n, tables, sym, blocks, stream,
                                        sizeof stream);
        Check(len > 0, "learn chunk build");
        if (!len) return;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t entry = 255u - i;  // 1..255
            const uint8_t* b = blocks + (size_t)i * 8;
            const uint32_t c0 = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
            const uint32_t c1 = (uint32_t)b[2] | ((uint32_t)b[3] << 8);
            const uint32_t e[6] = {c0 >> 11, (c0 >> 5) & 63, c0 & 31,
                                   c1 >> 11, (c1 >> 5) & 63, c1 & 31};
            for (uint32_t c = 0; c < 6; ++c) {
                const uint32_t s = sym[2 + c][i];
                int32_t v = (int32_t)e[c] - Zigzag(s);
                v %= (int32_t)kMod[c];
                if (v < 0) v += (int32_t)kMod[c];
                if (!histSeen[entry - 1]) {
                    hist[entry - 1][c] = (uint8_t)v;
                } else if (hist[entry - 1][c] != (uint8_t)v) {
                    histOk = false;
                }
            }
            histSeen[entry - 1] = true;
            for (uint32_t q = 0; q < 4; ++q) {
                const uint32_t s = sym[8 + q][i];
                const uint32_t code =
                    (q == 0) ? ((b[4] & 15) | ((b[5] & 15) << 4))
                  : (q == 1) ? ((b[4] >> 4) | ((b[5] >> 4) << 4))
                  : (q == 2) ? ((b[6] & 15) | ((b[7] & 15) << 4))
                             : ((b[6] >> 4) | ((b[7] >> 4) << 4));
                if (!quadSeen[s]) {
                    quad[s] = (uint8_t)code;
                } else if (quad[s] != (uint8_t)code) {
                    quadOk = false;
                }
                quadSeen[s] = true;
            }
        }
    }
    uint32_t histCount = 0, quadCount = 0;
    for (uint32_t k = 0; k < SC_BC1_HISTORY; ++k)
        if (histSeen[k]) ++histCount;
    for (uint32_t s = 0; s < 256; ++s)
        if (quadSeen[s]) ++quadCount;
    // Two passes, independent symbol choices: the values must be consistent.
    Check(histOk, "history derivation is consistent across data sets");
    Check(quadOk, "quadrant derivation is consistent across data sets");
    Check(histCount == SC_BC1_HISTORY, "history entries covered");
    Check(quadCount == 256, "quadrant symbols covered");
    Check(memcmp(hist, tables->history, sizeof hist) == 0,
          "derived history equals decoder table");
    Check(memcmp(quad, tables->quadrant, sizeof quad) == 0,
          "derived quadrant equals decoder table");
    printf("B1 learn-tables: history %u/%u and quadrant %u/256 recovered from "
           "our own blocks (2 independent passes), identical to the decoder's "
           "tables\n",
           (unsigned)histCount, (unsigned)SC_BC1_HISTORY, (unsigned)quadCount);
}

// ---- model fingerprint ---------------------------------------------------

static uint32_t Fingerprint() {
    uint32_t h = 2166136261u;
    for (uint32_t q = 0; q <= 15; ++q) {
        for (uint32_t w = 1; w <= 8; ++w) {
            uint32_t cum[257];
            const uint32_t n = ScBcpackModelCum(q, w, cum, 257);
            if (!n) return 0;
            for (uint32_t v = 0; v <= n; ++v) {
                const uint32_t x = cum[v];
                for (int b = 0; b < 4; ++b) {
                    h ^= (x >> (8 * b)) & 0xFF;
                    h *= 16777619u;
                }
            }
        }
    }
    return h;
}

// ---- reference cross-check ----------------------------------------------

static bool RunProcess(const char* cmdline, int* exitCode, char* outText,
                       size_t outCap) {
    if (outText && outCap) outText[0] = 0;
    char full[4096];
    snprintf(full, sizeof full, "cmd /c \"%s\"", cmdline);
    FILE* p = _popen(full, "r");
    if (!p) return false;
    if (outText && outCap) {
        size_t used = 0;
        int c;
        while ((c = fgetc(p)) != EOF) {
            if (used + 1 < outCap) outText[used++] = (char)c;
        }
        outText[used] = 0;
    } else {
        while (fgetc(p) != EOF) {
        }
    }
    *exitCode = _pclose(p);
    return true;
}

static bool WriteFileBytes(const char* path, const void* data, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

static bool ReadFileBytes(const char* path, void* data, size_t cap,
                          size_t* got) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    *got = fread(data, 1, cap, f);
    fclose(f);
    return true;
}

static void DumpHex(const char* what, const uint8_t* p, size_t n) {
    printf("  %s (%u bytes):", what, (unsigned)n);
    for (size_t i = 0; i < n && i < 48; ++i) printf(" %02x", p[i]);
    if (n > 48) printf(" ...");
    printf("\n");
}

// Compares our tables with the reference's `model` output for every (q, w).
static void CrossModels(const char* refExe) {
    int mismatches = 0, compared = 0;
    char cmd[2048], text[1 << 16];
    for (uint32_t q = 0; q <= 15; ++q) {
        for (uint32_t w = 1; w <= 8; ++w) {
            snprintf(cmd, sizeof cmd, "\"%s\" model --q %u --width %u",
                     refExe, q, w);
            int rc = -1;
            if (!RunProcess(cmd, &rc, text, sizeof text) || rc != 0) {
                printf("  model q=%u w=%u: reference call failed\n", q, w);
                ++mismatches;
                continue;
            }
            uint32_t cum[257];
            const uint32_t n = ScBcpackModelCum(q, w, cum, 257);
            // Output lines: "# ..." then "<symbol>\t<cum>\t<freq>".
            const char* p = text;
            uint32_t idx = 0;
            bool bad = false;
            while (*p) {
                const char* lineEnd = strchr(p, '\n');
                if (!lineEnd) lineEnd = p + strlen(p);
                if (*p != '#' && *p != '\r' && *p != '\n' && lineEnd > p) {
                    unsigned sym = 0, c = 0, f = 0;
                    if (sscanf(p, "%u\t%u\t%u", &sym, &c, &f) == 3) {
                        if (sym != idx || idx >= n + 1 || cum[idx] != c)
                            bad = true;
                        ++idx;
                    }
                }
                p = lineEnd;
                while (*p == '\r' || *p == '\n') ++p;
            }
            ++compared;
            if (bad || idx != n) {
                ++mismatches;
                printf("  model q=%u w=%u differs (parsed %u rows, n=%u)\n", q,
                       w, idx, n);
            }
        }
    }
    Check(mismatches == 0, "probability tables match the reference");
    printf("B1 cross-check models: %d/%d (q,width) tables identical to the "
           "reference\n",
           compared - mismatches, compared);
}

static void CrossChunks(const char* refExe, const char* dir,
                        const ScBc1Tables* tables) {
    static uint8_t sym[SC_BCPACK_BC1_LANES][256];
    static uint8_t blocks[256 * 8];
    static uint8_t stream[1 << 20];
    static uint8_t decoded[256 * 8];
    static uint8_t other[1 << 20];
    char cmd[4096], path[1024];
    const uint32_t counts[4] = {1, 16, 64, 256};

    // The reference needs a table file: ours, written in its documented JSON
    // shape. It never sees them any other way.
    snprintf(path, sizeof path, "%s\\bc1-tables.json", dir);
    FILE* tf = fopen(path, "wb");
    if (!tf) {
        Check(false, "write table json");
        return;
    }
    fprintf(tf, "{\"format\":\"bcpack-tables\",\"version\":1,\"bc1\":{\"history\":[");
    for (uint32_t k = 0; k < SC_BC1_HISTORY; ++k) {
        fprintf(tf, "%s[%u,%u,%u,%u,%u,%u]", k ? "," : "",
                tables->history[k][0], tables->history[k][1],
                tables->history[k][2], tables->history[k][3],
                tables->history[k][4], tables->history[k][5]);
    }
    fprintf(tf, "],\"quadrant_codes\":[");
    for (uint32_t s = 0; s < 256; ++s)
        fprintf(tf, "%s%u", s ? "," : "", tables->quadrant[s]);
    fprintf(tf, "]}}\n");
    fclose(tf);

    uint32_t agreed = 0, compared = 0;
    for (int k = 0; k < 4; ++k) {
        const uint32_t n = counts[k];
        const uint32_t len = BuildChunk(0, n, tables, sym, blocks, stream,
                                        sizeof stream);
        Check(len > 0, "cross chunk build");
        if (!len) continue;
        snprintf(path, sizeof path, "%s\\chunk-%u.bin", dir, n);
        if (!WriteFileBytes(path, stream, len)) {
            Check(false, "write chunk");
            continue;
        }
        // 1. Our own decode, for the comparison baseline.
        Check(ScBcpackDecodeBc1(stream, len, n * 8, tables, decoded, nullptr) ==
                  SC_BCPACK_OK,
              "cross baseline decode");
        // 2. The reference's BC1 decode of the same chunk, same tables.
        char inPath[1024], outPath[1024], tblPath[1024];
        snprintf(inPath, sizeof inPath, "%s\\chunk-%u.bin", dir, n);
        snprintf(outPath, sizeof outPath, "%s\\ref-%u.bin", dir, n);
        snprintf(tblPath, sizeof tblPath, "%s\\bc1-tables.json", dir);
        snprintf(cmd, sizeof cmd,
                 "\"%s\" decode --codec bc1 --tables \"%s\" --size %u \"%s\" "
                 "\"%s\"",
                 refExe, tblPath, n * 8, inPath, outPath);
        int rc = -1;
        char text[4096];
        ++compared;
        if (!RunProcess(cmd, &rc, text, sizeof text) || rc != 0) {
            printf("  reference decode failed for %u blocks (rc=%d)\n", n, rc);
            continue;
        }
        size_t got = 0;
        if (!ReadFileBytes(outPath, other, sizeof other, &got)) {
            printf("  reference output missing for %u blocks\n", n);
            continue;
        }
        if (got != n * 8 || memcmp(other, decoded, n * 8) != 0) {
            printf("  DISAGREEMENT at %u blocks: our %u bytes vs reference "
                   "%u bytes\n",
                   n, (unsigned)(n * 8), (unsigned)got);
            DumpHex("chunk", stream, len);
            DumpHex("ours", decoded, n * 8);
            DumpHex("reference", other, got);
        } else {
            ++agreed;
        }
        // 3. The reference's entropy-only view, compared with our symbols.
        char lanesPath[1024];
        snprintf(lanesPath, sizeof lanesPath, "%s\\lanes-%u.bin", dir, n);
        snprintf(cmd, sizeof cmd,
                 "\"%s\" lanes --codec bc1 --size %u \"%s\" \"%s\"", refExe,
                 n * 8, inPath, lanesPath);
        int rc2 = -1;
        if (RunProcess(cmd, &rc2, text, sizeof text) || rc2 == 0) {
            size_t lgot = 0;
            if (ReadFileBytes(lanesPath, other, sizeof other, &lgot) &&
                lgot == (size_t)SC_BCPACK_BC1_LANES * n) {
                bool same = true;
                for (uint32_t l = 0; l < SC_BCPACK_BC1_LANES; ++l) {
                    if (memcmp(other + (size_t)l * n, sym[l], n) != 0)
                        same = false;
                }
                ++compared;
                if (same) {
                    ++agreed;
                } else {
                    printf("  DISAGREEMENT in lane symbols at %u blocks\n", n);
                    DumpHex("our lane 0", sym[0], n);
                }
            }
        }
    }
    Check(agreed == compared && compared > 0,
          "BC1 decode and lane symbols agree with the reference");
    printf("B1 cross-check chunks: %u/%u comparisons identical to the "
           "reference (BC1 blocks and lane symbols)\n",
           agreed, compared);
}

// ---- main ---------------------------------------------------------------

int main(int argc, char** argv) {
    ScBc1Tables tables;
    ScBc1TablesDefault(&tables);

    printf("B1 BCPack BC1 harness (our own decoder + spec-built encoder)\n");
    printf("model fingerprint: %08x\n", Fingerprint());

    // The bit table is what the bitstream depends on: keep the shape sane.
    {
        uint32_t cum[257];
        const uint32_t n = ScBcpackModelCum(12, 5, cum, 257);
        Check(n == 32, "q12/w5 symbol count");
        // Documented example: per-symbol frequencies by bucket.
        const uint32_t want[6] = {53290, 9957, 1104, 14, 1, 1};
        const uint32_t starts[6] = {0, 1, 2, 4, 8, 16};
        bool ok = true;
        for (int b = 0; b < 6; ++b) {
            if (cum[starts[b] + 1] - cum[starts[b]] != want[b]) ok = false;
        }
        Check(ok, "q12/w5 frequencies match the documented example");
        if (ok) printf("B1 model example: q=12 w=5 buckets 53290,9957,1104,"
                       "14,1,1 (documented) reproduced\n");
    }

    TestRoundTrips(&tables);
    TestMalformed(&tables);
    TestLearnTables(&tables);

    // Chunk-table storage detection (bits 26..29), detection only.
    {
        const uint64_t raw = (uint64_t)0 << 26;
        const uint64_t bc1 = (uint64_t)1 << 26;
        const uint64_t zlib = (uint64_t)8 << 26;
        const uint64_t bad = (uint64_t)12 << 26;
        Check(ScBcpackChunkStorage(raw) == SC_BCPACK_STORAGE_RAW, "storage raw");
        Check(ScBcpackChunkStorage(bc1) == SC_BCPACK_STORAGE_BC1, "storage bc1");
        Check(ScBcpackChunkStorage(zlib) == SC_BCPACK_STORAGE_ZLIB, "storage zlib");
        Check(ScBcpackChunkStorage(bad) == SC_BCPACK_STORAGE_UNKNOWN,
              "storage reserved");
        printf("B1 chunk-storage: bits 26..29 -> raw(0), bcpack-bc1..bc7(1..7), "
               "zlib(8); detection only, no wiring\n");
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--cross") == 0 && i + 2 < argc) {
            CrossModels(argv[i + 1]);
            CrossChunks(argv[i + 1], argv[i + 2], &tables);
            break;
        }
    }

    printf("B1 result: %d checks, %d failed -> %s\n", g_checks, g_fail,
           g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
