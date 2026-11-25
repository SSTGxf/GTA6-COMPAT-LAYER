// bcpack.cpp — BCPack milestone B1: the 12-lane rANS entropy layer and the
// BC1 block rebuilder, implemented from the published reconstruction (record
// layout, generated probability models, scheduler, BC1 lane semantics). No
// code or table is taken from another BCPack implementation: the BC1 tables
// are the caller's (see ScBc1Tables) and every number here is either in the
// format description or derived by ScBcpackModelCum.
//
// WHY THE SCHEDULER IS A SIMULATION: BCPack has no chunk header, no lane
// directory and no length prefix. The lanes share one byte stream and bytes
// are handed out by a round-based scheduler whose decisions depend on the
// bytes themselves (a lane that asks for a byte and is not served does not
// decode that round). So decoding is a coupled state machine, and the test
// harness's encoder uses the SAME machine with per-lane byte queues to learn
// the order in which the decoder will fetch the bytes. That is why
// ScBcpackBuildStream exists: it is the encoder side of this one scheduler,
// not a second implementation.
//
// STRICTNESS: every record must end with its rANS state back at L exactly,
// every lane must carry exactly outBytes/8 symbols, no record may have unread
// bytes, and the chunk must be fully consumed. Any deviation fails the call
// and the destination buffer is not written.

#include "bcpack.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// rANS constants from the format: scale 2^16, normalised interval [L, 256L).
static const uint32_t kScale = 1u << 16;
static const uint32_t kL = 1u << 24;

// Lane layouts. Base widths, one symbol per coding unit (8 output bytes).
// BC1 is B1; BC3..BC7 are the B2 entropy-only layouts from the format
// description (BC7's lane 15 is 5 bits wide, not 8). BC2 has no published
// layout and is rejected.
typedef struct CodecLayout {
    uint32_t lanes;
    uint32_t widths[SC_BCPACK_MAX_LANES];
    uint32_t unitsPerBlock;
    uint32_t blockBytes;
} CodecLayout;

static const CodecLayout kLayouts[8] = {
    {0, {0}, 0, 0},                                             // 0 unused
    {12, {8, 8, 5, 6, 5, 5, 6, 5, 8, 8, 8, 8}, 1, 8},           // BC1
    {0, {0}, 0, 0},                                             // BC2
    {12, {4, 4, 8, 6, 6, 6, 6, 5, 6, 5, 8, 8}, 2, 16},          // BC3
    {12, {8, 8, 8, 8, 6, 6, 6, 6, 6, 6, 6, 6}, 1, 8},           // BC4
    {12, {4, 4, 8, 8, 6, 6, 6, 6, 6, 6, 6, 6}, 2, 16},          // BC5
    {16, {4, 4, 8, 8, 8, 5, 5, 5, 6, 6, 6, 8, 8, 8, 8, 5}, 2, 16},  // BC6H
    {16, {4, 4, 8, 8, 8, 8, 8, 7, 7, 5, 6, 8, 8, 8, 8, 5}, 2, 16},  // BC7
};

// BC1 lane widths, used by the block assembler.
static const uint32_t* kWidths = kLayouts[SC_BCPACK_CODEC_BC1].widths;

static const CodecLayout* LayoutFor(uint32_t codec) {
    if (codec < 1 || codec > 7) return nullptr;
    const CodecLayout* L = &kLayouts[codec];
    return L->lanes ? L : nullptr;
}

uint32_t WINAPI ScBcpackCodecLayout(uint32_t codec, ScBcpackCodecInfo* out) {
    const CodecLayout* L = LayoutFor(codec);
    if (!L) return 0;
    if (!out) return 1;
    memset(out, 0, sizeof *out);
    out->codec = codec;
    out->lanes = L->lanes;
    memcpy(out->widths, L->widths, sizeof out->widths);
    out->unitsPerBlock = L->unitsPerBlock;
    out->blockBytes = L->blockBytes;
    return 1;
}

// Endpoint moduli for {R0,G0,B0,R1,G1,B1}.
static const uint32_t kMod[6] = {32, 64, 32, 32, 64, 32};

// ---- probability models --------------------------------------------------

// Fitted q parameters (the format table, section "Probability model"). The
// integer tables these produce are what the bitstream depends on.
static double LambdaForQ(uint32_t q) {
    static const double kLambda[16] = {
        0.0,        0.7399434, 1.4962494, 2.30715745, 3.22101565, 4.3073742,
        5.6759508,  7.50811,   10.1124375, 14.03737,  20.35237,  31.4462,
        53.67977,   109.2579,  328.076,   3000.0};
    return q < 16 ? kLambda[q] : 0.0;
}

// IEEE roundTiesToEven; nearbyint uses the default rounding mode.
static double RoundHalfEven(double v) { return nearbyint(v); }

typedef struct Model {
    uint32_t n;           // symbols (2^width)
    uint32_t cum[257];    // cum[0] = 1 (FIN boundary), cum[n] = 65536
} Model;

// Builds model (q, width). layout:
//   q = 0: cum[0] = 1, cum[v] = v * 2^(16-width)  (symbol 0 owns one slot
//          fewer than the rest: the FIN slot is not a symbol)
//   q = 1..15: truncated geometric law over the buckets {0}, {1}, {2,3},
//          {4..7}, ... {n/2..n-1}
// Returns false when the result is not a usable table (never expected).
static bool BuildModel(uint32_t q, uint32_t width, Model* m) {
    if (width < 1 || width > 8 || q > 15) return false;
    const uint32_t n = 1u << width;
    memset(m, 0, sizeof *m);
    m->n = n;
    if (q == 0) {
        m->cum[0] = 1;
        for (uint32_t v = 1; v <= n; ++v) m->cum[v] = v << (16 - width);
        return m->cum[n] == 65536;
    }
    const double lambda = LambdaForQ(q);
    const double rho = exp(-lambda / (double)n);
    const double rn = pow(rho, (double)n);
    double denom = 1.0 - rn;
    if (!(denom > 1e-12)) denom = 1.0;  // saturated q: every symbol gets 1

    uint32_t start[10], size[10], boundary[12], freq[10];
    start[0] = 0;
    size[0] = 1;
    for (uint32_t k = 1; k <= width; ++k) {
        start[k] = 1u << (k - 1);
        size[k] = start[k];  // bucket k spans [2^(k-1), 2^k)
    }
    // boundary[k] is the first slot of bucket k; boundary[width + 1] is the
    // end of the table (65536).
    for (uint32_t k = 1; k <= width; ++k) {
        double f = (1.0 - pow(rho, (double)start[k])) / denom;
        double b = RoundHalfEven(65536.0 * f);
        if (b < 1.0) b = 1.0;
        if (b > 65536.0) b = 65536.0;
        boundary[k] = (uint32_t)b;
    }
    boundary[0] = 1;
    boundary[width + 1] = 65536;
    // Top-down so that every bucket keeps at least one slot per symbol.
    for (uint32_t k = width; k >= 1; --k) {
        const uint32_t bmax = boundary[k + 1] - size[k];
        if (boundary[k] > bmax) boundary[k] = bmax;
        double raw = (double)(boundary[k + 1] - boundary[k]) / (double)size[k];
        uint32_t f = (uint32_t)RoundHalfEven(raw);
        if (f < 1) f = 1;
        boundary[k] = boundary[k + 1] - f * size[k];
        freq[k] = f;
        if (k == 1) break;
    }
    freq[0] = boundary[1] - boundary[0];
    if (freq[0] < 1) freq[0] = 1;

    m->cum[0] = 1;
    for (uint32_t k = 1; k <= width; ++k) {
        for (uint32_t j = 0; j < size[k]; ++j) {
            m->cum[start[k] + j] = boundary[k] + j * freq[k];
        }
    }
    m->cum[n] = 65536;
    // A usable table is strictly increasing below 65536.
    for (uint32_t v = 0; v < n; ++v) {
        if (m->cum[v] >= m->cum[v + 1]) return false;
    }
    return m->cum[n] == 65536;
}

uint32_t WINAPI ScBcpackModelCum(uint32_t q, uint32_t width, uint32_t* cum,
                                 uint32_t cap) {
    if (!cum || width < 1 || width > 8 || q > 15) return 0;
    Model m;
    if (!BuildModel(q, width, &m)) return 0;
    if (cap < m.n + 1) return 0;
    memcpy(cum, m.cum, (m.n + 1) * sizeof(uint32_t));
    return m.n;
}

// Largest v with cum[v] <= slot (slot >= 1, so v >= 0).
static uint32_t LookupSymbol(const Model* m, uint32_t slot) {
    uint32_t lo = 0, hi = m->n;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (m->cum[mid] <= slot)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

// ---- scheduler -----------------------------------------------------------

enum LanePhase { PH_HEADER = 0, PH_RUN = 1, PH_DONE = 2 };

typedef struct Lane {
    uint32_t baseWidth;
    uint32_t phase;
    uint8_t hdr[8];
    uint32_t hdrLen;
    uint32_t total;   // record bytes after the length field (state included)
    uint32_t q, w;
    Model model;
    uint32_t x;
    int32_t buf;      // at most one fetched byte not merged yet, -1 = none
    uint32_t budget;  // payload bytes not yet fetched
    uint32_t pending; // decoded ahead of the unit counter
    bool fresh;
    bool stalled;
    uint32_t symbols;
} Lane;

typedef struct Source {
    // decode mode
    const uint8_t* stream;
    uint32_t streamLen, streamPos;
    // record mode (harness encoder): one byte queue per lane
    const uint8_t* queue[SC_BCPACK_MAX_LANES];
    uint32_t queueLen[SC_BCPACK_MAX_LANES];
    uint32_t queuePos[SC_BCPACK_MAX_LANES];
    uint8_t* out;
    uint32_t outCap, outLen;
    bool recording;
    bool starved;  // a lane asked for a byte that was not available
} Source;

static bool TakeByte(Source* s, uint32_t lane, uint8_t* out) {
    if (s->recording) {
        if (s->queuePos[lane] >= s->queueLen[lane]) {
            s->starved = true;
            return false;
        }
        if (s->outLen >= s->outCap) {
            s->starved = true;
            return false;
        }
        const uint8_t b = s->queue[lane][s->queuePos[lane]++];
        s->out[s->outLen++] = b;
        *out = b;
        return true;
    }
    if (s->streamPos >= s->streamLen) {
        s->starved = true;
        return false;
    }
    *out = s->stream[s->streamPos++];
    return true;
}

// Parses the header bytes collected so far. Returns 0 while more bytes are
// needed, 1 when the lane entered Run, 2 on a malformed header.
static uint32_t HeaderStep(Lane* L) {
    if (L->hdrLen < 2) return 0;
    const uint32_t mode = L->hdr[0];
    const uint32_t ext = (mode >> 5) & 1;
    const uint32_t need = ext ? 3u : 2u;
    if (L->hdrLen < need) return 0;
    const uint32_t q = mode & 0xFu;
    const uint32_t r = mode >> 6;
    if (q > 15 || r >= L->baseWidth) return 2;  // width would be 0
    const uint32_t w = L->baseWidth - r;
    uint32_t total;
    if (ext) {
        total = ((uint32_t)L->hdr[1] | ((uint32_t)L->hdr[2] << 8)) + 257u;
    } else {
        total = (uint32_t)L->hdr[1] + 1u;
    }
    const uint32_t nst = total < 4u ? total : 4u;
    if (L->hdrLen < need + nst) return 0;
    if (!BuildModel(q, w, &L->model)) return 2;
    uint32_t x = 0;
    for (uint32_t i = 0; i < nst; ++i) x = (x << 8) | L->hdr[need + i];
    L->q = q;
    L->w = w;
    L->total = total;
    L->x = x;
    L->budget = total - nst;
    L->buf = -1;
    L->pending = 0;
    L->phase = PH_RUN;
    L->fresh = true;
    L->hdrLen = 0;
    return 1;
}

// The record-end condition: no payload bytes left, no fetched byte waiting,
// the state is normalised, and the next symbol would be FIN (slot 0) or the
// state is back at the encoder's initial state.
static bool RecordEnded(const Lane* L) {
    if (L->budget != 0 || L->buf >= 0) return false;
    if (L->x < kL) return false;
    return L->x == kL || (L->x & 0xFFFFu) == 0;
}

// Runs the shared byte-scheduler. symOut (when non-null) receives
// lane-major symbols: symOut[lane * units + index].
static uint32_t RunScheduler(Source* src, const CodecLayout* layout,
                             uint32_t outBytes, uint8_t* symOut,
                             ScBcpackDiag* diag) {
    const uint32_t units = outBytes / SC_BCPACK_BC1_UNIT_BYTES;
    const uint32_t laneCount = layout->lanes;
    Lane lanes[SC_BCPACK_MAX_LANES];
    memset(lanes, 0, sizeof lanes);
    for (uint32_t l = 0; l < laneCount; ++l) {
        lanes[l].baseWidth = layout->widths[l];
        lanes[l].phase = PH_HEADER;
        lanes[l].buf = -1;
    }
    uint32_t unitsDone = 0;
    uint32_t rounds = 0;
    const uint32_t limit = 20u * units + 200u;

    for (;;) {
        bool allDone = true;
        for (uint32_t l = 0; l < laneCount; ++l) {
            if (lanes[l].phase != PH_DONE) allDone = false;
        }
        if (allDone) break;
        if (rounds >= limit) {
            if (diag) diag->rounds = rounds;
            return SC_BCPACK_ROUND_LIMIT;
        }
        ++rounds;

        // 1. Requests: the first 8 lanes (by number) that want a byte.
        bool want[SC_BCPACK_MAX_LANES] = {false};
        for (uint32_t l = 0; l < laneCount; ++l) {
            const Lane* L = &lanes[l];
            if (L->phase == PH_HEADER) {
                want[l] = true;
            } else if (L->phase == PH_RUN && L->buf < 0 && L->budget > 0 &&
                       (L->x < kL || L->pending == 0)) {
                want[l] = true;
            }
        }
        uint32_t serve[8];
        uint32_t nserve = 0;
        bool served[SC_BCPACK_MAX_LANES] = {false};
        for (uint32_t l = 0; l < laneCount && nserve < 8; ++l) {
            if (want[l]) {
                serve[nserve++] = l;
                served[l] = true;
            }
        }
        for (uint32_t l = 0; l < laneCount; ++l) {
            lanes[l].stalled =
                want[l] && !served[l] && lanes[l].phase == PH_RUN;
        }

        // 2. Reads, in increasing lane number.
        for (uint32_t i = 0; i < nserve; ++i) {
            Lane* L = &lanes[serve[i]];
            uint8_t b;
            if (!TakeByte(src, serve[i], &b)) {
                if (diag) diag->rounds = rounds;
                return SC_BCPACK_TRUNCATED;
            }
            if (L->phase == PH_HEADER) {
                if (L->hdrLen >= sizeof L->hdr) return SC_BCPACK_BAD_HEADER;
                L->hdr[L->hdrLen++] = b;
                const uint32_t st = HeaderStep(L);
                if (st == 2) return SC_BCPACK_BAD_HEADER;
            } else if (L->phase == PH_RUN) {
                L->buf = b;
                if (L->budget == 0) return SC_BCPACK_BAD_END;  // over budget
                --L->budget;
            }
        }

        // 3. Decoding, in lane order.
        for (uint32_t l = 0; l < laneCount; ++l) {
            Lane* L = &lanes[l];
            if (L->phase != PH_RUN) continue;
            if (L->fresh) {
                L->fresh = false;
                continue;
            }
            if (L->x < kL) {
                if (L->buf >= 0) {
                    L->x = (L->x << 8) | (uint32_t)L->buf;
                    L->buf = -1;
                    continue;
                }
                if (L->budget == 0) {
                    while (L->x < kL) L->x <<= 8;  // restored zero bytes
                    continue;
                }
                continue;  // waiting for its next byte
            }
            if (RecordEnded(L)) {
                if (L->x != kL) return SC_BCPACK_BAD_END;
                if (L->symbols == units) {
                    L->phase = PH_DONE;
                    L->pending = 0;
                    continue;
                }
                L->phase = PH_HEADER;
                L->hdrLen = 0;
                L->buf = -1;
                L->pending = 0;
                L->fresh = false;
                continue;
            }
            if (L->pending) continue;
            if (L->symbols == units) {
                // The record still has symbols but the output is complete.
                return SC_BCPACK_BAD_SIZE;
            }
            if (L->stalled) continue;
            const uint32_t slot = L->x & 0xFFFFu;
            if (slot == 0) return SC_BCPACK_BAD_SYMBOL;  // FIN mid-record
            const uint32_t v = LookupSymbol(&L->model, slot);
            if (v >= L->model.n) return SC_BCPACK_BAD_SYMBOL;
            const uint32_t f = L->model.cum[v + 1] - L->model.cum[v];
            L->x = f * (L->x >> 16) + slot - L->model.cum[v];
            if (L->x < kL && L->buf >= 0) {
                L->x = (L->x << 8) | (uint32_t)L->buf;
                L->buf = -1;
            }
            if (L->budget == 0 && L->buf < 0 && L->x < kL) {
                while (L->x < kL) L->x <<= 8;
            }
            if (symOut) symOut[l * units + L->symbols] = (uint8_t)v;
            ++L->symbols;
            L->pending = 1;
            if (RecordEnded(L)) {
                if (L->x != kL) return SC_BCPACK_BAD_END;
                if (L->symbols == units) {
                    L->phase = PH_DONE;
                    L->pending = 0;
                } else {
                    L->phase = PH_HEADER;
                    L->hdrLen = 0;
                    L->buf = -1;
                    L->pending = 0;
                }
            }
        }

        // 4. Unit synchronisation.
        bool sync = true;
        for (uint32_t l = 0; l < laneCount; ++l) {
            const Lane* L = &lanes[l];
            const bool ok = (L->pending == 1) ||
                            (L->phase == PH_DONE && L->symbols > unitsDone);
            if (!ok) sync = false;
        }
        if (sync) {
            for (uint32_t l = 0; l < laneCount; ++l)
                lanes[l].pending = 0;
            ++unitsDone;
        }
    }

    // Post-conditions: every lane carried exactly its symbols, no record bytes
    // are left over, and (decoding) the chunk was fully consumed.
    for (uint32_t l = 0; l < laneCount; ++l) {
        if (lanes[l].symbols != units) return SC_BCPACK_BAD_SIZE;
        if (lanes[l].budget != 0 || lanes[l].buf >= 0)
            return SC_BCPACK_TRAILING;
        if (diag) diag->symbols[l] = lanes[l].symbols;
    }
    if (diag) {
        diag->rounds = rounds;
        diag->units = unitsDone;
        diag->bytesConsumed = src->recording ? src->outLen : src->streamPos;
    }
    if (!src->recording && src->streamPos != src->streamLen)
        return SC_BCPACK_TRAILING;
    if (src->recording) {
        for (uint32_t l = 0; l < laneCount; ++l) {
            if (src->queuePos[l] != src->queueLen[l])
                return SC_BCPACK_TRAILING;
        }
    }
    return SC_BCPACK_OK;
}

// ---- BC1 block assembly --------------------------------------------------

static uint32_t AssembleBc1(const uint8_t* sym, uint32_t units,
                            const ScBc1Tables* tables, uint8_t* dst) {
    uint8_t ring[256][6];
    memset(ring, 0, sizeof ring);
    uint8_t e[6];
    for (uint32_t i = 0; i < units; ++i) {
        const uint32_t v0 = sym[0 * units + i];
        if (sym[1 * units + i] != 0) return SC_BCPACK_BAD_SYMBOL;
        uint8_t pred[6] = {0, 0, 0, 0, 0, 0};
        if (v0 != 0) {
            const uint32_t d = (v0 & 1u) ? (255u - (v0 >> 1)) : (v0 >> 1);
            if (d == 0) return SC_BCPACK_BAD_SYMBOL;
            if (d <= i) {
                memcpy(pred, ring[(i - d) & 255u], 6);
            } else {
                const uint32_t entry = d - i;  // 1..255 -> history[entry-1]
                if (entry > SC_BC1_HISTORY) return SC_BCPACK_BAD_SYMBOL;
                for (uint32_t c = 0; c < 6; ++c)
                    pred[c] = tables->history[entry - 1][c];
            }
        }
        for (uint32_t c = 0; c < 6; ++c) {
            const uint32_t s = sym[(2 + c) * units + i];
            if (s >= (1u << kWidths[2 + c])) return SC_BCPACK_BAD_SYMBOL;
            const int32_t zz =
                (s & 1u) ? -(int32_t)(s >> 1) - 1 : (int32_t)(s >> 1);
            int32_t val = (int32_t)pred[c] + zz;
            const int32_t m = (int32_t)kMod[c];
            val %= m;
            if (val < 0) val += m;
            e[c] = (uint8_t)val;
        }
        memcpy(ring[i & 255u], e, 6);
        const uint32_t c0 =
            ((uint32_t)e[0] << 11) | ((uint32_t)e[1] << 5) | e[2];
        const uint32_t c1 =
            ((uint32_t)e[3] << 11) | ((uint32_t)e[4] << 5) | e[5];
        const uint32_t tl = tables->quadrant[sym[8 * units + i]];
        const uint32_t tr = tables->quadrant[sym[9 * units + i]];
        const uint32_t bl = tables->quadrant[sym[10 * units + i]];
        const uint32_t br = tables->quadrant[sym[11 * units + i]];
        uint8_t* b = dst + (size_t)i * 8;
        b[0] = (uint8_t)(c0 & 0xFF);
        b[1] = (uint8_t)(c0 >> 8);
        b[2] = (uint8_t)(c1 & 0xFF);
        b[3] = (uint8_t)(c1 >> 8);
        b[4] = (uint8_t)((tl & 15) | ((tr & 15) << 4));
        b[5] = (uint8_t)((tl >> 4) | ((tr >> 4) << 4));
        b[6] = (uint8_t)((bl & 15) | ((br & 15) << 4));
        b[7] = (uint8_t)((bl >> 4) | ((br >> 4) << 4));
    }
    return SC_BCPACK_OK;
}

// ---- public entry points -------------------------------------------------

void WINAPI ScBc1TablesDefault(ScBc1Tables* tables) {
    if (!tables) return;
    memset(tables->history, 0, sizeof tables->history);
    for (uint32_t s = 0; s < 256; ++s) tables->quadrant[s] = (uint8_t)s;
}

const char* WINAPI ScBcpackStatusName(uint32_t status) {
    switch (status) {
    case SC_BCPACK_OK: return "OK";
    case SC_BCPACK_BAD_PARAMS: return "BAD_PARAMS";
    case SC_BCPACK_BAD_SIZE: return "BAD_SIZE";
    case SC_BCPACK_TRUNCATED: return "TRUNCATED";
    case SC_BCPACK_BAD_HEADER: return "BAD_HEADER";
    case SC_BCPACK_BAD_SYMBOL: return "BAD_SYMBOL";
    case SC_BCPACK_BAD_END: return "BAD_END";
    case SC_BCPACK_TRAILING: return "TRAILING";
    case SC_BCPACK_ROUND_LIMIT: return "ROUND_LIMIT";
    case SC_BCPACK_NO_OUTPUT: return "NO_OUTPUT";
    case SC_BCPACK_UNSUPPORTED_CODEC: return "UNSUPPORTED_CODEC";
    default: return "?";
    }
}

uint32_t WINAPI ScBcpackChunkStorage(uint64_t entry) {
    const uint32_t v = (uint32_t)((entry >> 26) & 0xFu);
    return v <= SC_BCPACK_STORAGE_ZLIB ? v : SC_BCPACK_STORAGE_UNKNOWN;
}

const char* WINAPI ScBcpackChunkStorageName(uint32_t storage) {
    switch (storage) {
    case SC_BCPACK_STORAGE_RAW: return "raw";
    case SC_BCPACK_STORAGE_BC1: return "bcpack-bc1";
    case SC_BCPACK_STORAGE_BC2: return "bcpack-bc2";
    case SC_BCPACK_STORAGE_BC3: return "bcpack-bc3";
    case SC_BCPACK_STORAGE_BC4: return "bcpack-bc4";
    case SC_BCPACK_STORAGE_BC5: return "bcpack-bc5";
    case SC_BCPACK_STORAGE_BC6H: return "bcpack-bc6h";
    case SC_BCPACK_STORAGE_BC7: return "bcpack-bc7";
    case SC_BCPACK_STORAGE_ZLIB: return "zlib";
    default: return "unknown";
    }
}

// Shared decoder: fills sym (lanes * units bytes) and validates the chunk.
static uint32_t DecodeInto(const CodecLayout* layout, const void* src,
                           uint32_t srcBytes, uint32_t outBytes,
                           uint8_t* sym, ScBcpackDiag* diag) {
    Source s;
    memset(&s, 0, sizeof s);
    s.stream = (const uint8_t*)src;
    s.streamLen = srcBytes;
    const uint32_t st = RunScheduler(&s, layout, outBytes, sym, diag);
    if (diag) {
        diag->lanes = layout->lanes;
        diag->status = st;
    }
    return st;
}

uint32_t WINAPI ScBcpackDecodeLanes(uint32_t codec, const void* src,
                                   uint32_t srcBytes, uint32_t outBytes,
                                   void* laneSymbols, ScBcpackDiag* diag) {
    if (diag) memset(diag, 0, sizeof *diag);
    const CodecLayout* layout = LayoutFor(codec);
    if (!src || !laneSymbols) {
        if (diag) diag->status = SC_BCPACK_BAD_PARAMS;
        return SC_BCPACK_BAD_PARAMS;
    }
    if (!layout) {
        if (diag) diag->status = SC_BCPACK_UNSUPPORTED_CODEC;
        return SC_BCPACK_UNSUPPORTED_CODEC;
    }
    if (outBytes == 0 || (outBytes % SC_BCPACK_BC1_UNIT_BYTES) != 0) {
        if (diag) diag->status = SC_BCPACK_BAD_SIZE;
        return SC_BCPACK_BAD_SIZE;
    }
    const uint32_t units = outBytes / SC_BCPACK_BC1_UNIT_BYTES;
    const size_t total = (size_t)layout->lanes * units;
    uint8_t* sym = (uint8_t*)calloc(total, 1);
    if (!sym) {
        if (diag) diag->status = SC_BCPACK_BAD_PARAMS;
        return SC_BCPACK_BAD_PARAMS;
    }
    const uint32_t st = DecodeInto(layout, src, srcBytes, outBytes, sym, diag);
    if (st == SC_BCPACK_OK) memcpy(laneSymbols, sym, total);  // fail closed
    free(sym);
    return st;
}

uint32_t WINAPI ScBcpackDecodeBc1(const void* src, uint32_t srcBytes,
                                  uint32_t outBytes,
                                  const ScBc1Tables* tables, void* dst,
                                  ScBcpackDiag* diag) {
    if (diag) memset(diag, 0, sizeof *diag);
    if (!src || !dst || !tables) {
        if (diag) diag->status = SC_BCPACK_BAD_PARAMS;
        return SC_BCPACK_BAD_PARAMS;
    }
    if (outBytes == 0 || (outBytes % SC_BCPACK_BC1_UNIT_BYTES) != 0) {
        if (diag) diag->status = SC_BCPACK_BAD_SIZE;
        return SC_BCPACK_BAD_SIZE;
    }
    const uint32_t units = outBytes / SC_BCPACK_BC1_UNIT_BYTES;
    const CodecLayout* layout = &kLayouts[SC_BCPACK_CODEC_BC1];
    uint8_t* sym =
        (uint8_t*)calloc((size_t)SC_BCPACK_BC1_LANES * units, 1);
    uint8_t* out = (uint8_t*)malloc(outBytes);
    if (!sym || !out) {
        free(sym);
        free(out);
        if (diag) diag->status = SC_BCPACK_BAD_PARAMS;
        return SC_BCPACK_BAD_PARAMS;
    }
    uint32_t st = DecodeInto(layout, src, srcBytes, outBytes, sym, diag);
    if (st == SC_BCPACK_OK) st = AssembleBc1(sym, units, tables, out);
    if (st == SC_BCPACK_OK) memcpy(dst, out, outBytes);  // fail closed
    if (diag) diag->status = st;
    free(sym);
    free(out);
    return st;
}

static uint32_t BuildStreamCore(
    const CodecLayout* layout,
    const uint8_t* const laneBytes[SC_BCPACK_MAX_LANES],
    const uint32_t laneLens[SC_BCPACK_MAX_LANES], uint32_t outBytes,
    void* stream, uint32_t streamCap, uint32_t* streamLen,
    ScBcpackDiag* diag) {
    if (diag) memset(diag, 0, sizeof *diag);
    if (!layout || !laneBytes || !laneLens || !stream || !streamLen) {
        if (diag) diag->status = SC_BCPACK_BAD_PARAMS;
        return SC_BCPACK_BAD_PARAMS;
    }
    if (outBytes == 0 || (outBytes % SC_BCPACK_BC1_UNIT_BYTES) != 0) {
        if (diag) diag->status = SC_BCPACK_BAD_SIZE;
        return SC_BCPACK_BAD_SIZE;
    }
    Source s;
    memset(&s, 0, sizeof s);
    s.recording = true;
    for (uint32_t l = 0; l < layout->lanes; ++l) {
        s.queue[l] = laneBytes[l];
        s.queueLen[l] = laneLens[l];
    }
    s.out = (uint8_t*)stream;
    s.outCap = streamCap;
    const uint32_t st = RunScheduler(&s, layout, outBytes, nullptr, diag);
    if (st == SC_BCPACK_OK) *streamLen = s.outLen;
    if (diag) {
        diag->lanes = layout->lanes;
        diag->status = st;
    }
    return st;
}

uint32_t WINAPI ScBcpackBuildStream(
    const uint8_t* const laneBytes[SC_BCPACK_BC1_LANES],
    const uint32_t laneLens[SC_BCPACK_BC1_LANES], uint32_t outBytes,
    void* stream, uint32_t streamCap, uint32_t* streamLen,
    ScBcpackDiag* diag) {
    const uint8_t* lane[SC_BCPACK_MAX_LANES] = {0};
    uint32_t lens[SC_BCPACK_MAX_LANES] = {0};
    for (uint32_t l = 0; l < SC_BCPACK_BC1_LANES; ++l) {
        lane[l] = laneBytes ? laneBytes[l] : nullptr;
        lens[l] = laneLens ? laneLens[l] : 0;
    }
    return BuildStreamCore(&kLayouts[SC_BCPACK_CODEC_BC1], lane, lens,
                           outBytes, stream, streamCap, streamLen, diag);
}

uint32_t WINAPI ScBcpackBuildStreamEx(
    uint32_t codec, const uint8_t* const laneBytes[SC_BCPACK_MAX_LANES],
    const uint32_t laneLens[SC_BCPACK_MAX_LANES], uint32_t outBytes,
    void* stream, uint32_t streamCap, uint32_t* streamLen,
    ScBcpackDiag* diag) {
    const CodecLayout* layout = LayoutFor(codec);
    if (!layout) {
        if (diag) {
            memset(diag, 0, sizeof *diag);
            diag->status = SC_BCPACK_UNSUPPORTED_CODEC;
        }
        return SC_BCPACK_UNSUPPORTED_CODEC;
    }
    return BuildStreamCore(layout, laneBytes, laneLens, outBytes, stream,
                           streamCap, streamLen, diag);
}
