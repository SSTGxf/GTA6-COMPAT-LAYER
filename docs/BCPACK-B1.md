# BCPack milestones B1 and B2 — the entropy layer and the BC1 rebuilder

Scope: `docs/FREEBUFF-BCPACK-TEXTURE-SPEC.md` Part 1 (B1) plus the B2
extension: entropy-only lane layouts for BC3, BC4, BC5, BC6H and BC7. No
block semantics for those formats, no BC6H history calibration, no
DirectStorage wiring, no game data, no GPU.

Files: `include/bcpack.h`, `src/bcpack.cpp`, `tests/bcpack_b1.cpp` (harness,
including the only encoder in this tree). The module is built and run by
`build.bat` stage 3b; nothing in the layer calls it yet.

## What is implemented

Decoding one BC1 chunk (`ScBcpackDecodeBc1`):

- **Record layout.** `mode, len_lo, [len_hi], state[0..min(4,total))`,
  `total = len_lo+1` or `(len_lo | len_hi<<8)+257`; `mode` bits 0–3 = model
  `q`, bit 4 = flush, bit 5 = extended length, bits 6–7 = width reduction
  `r` (`w' = w - r`). Trailing zero bytes are not stored and are restored as
  zero bytes when the payload is exhausted.
- **rANS.** Scale `M = 2^16`, interval `[L, 256L)` with `L = 2^24`, big-endian
  state, `slot = x & 0xFFFF`, `x = freq*(x>>16) + slot - cum`. Slot 0 of every
  table is FIN and belongs to no symbol.
- **Probability models** (generated, not transmitted). `q = 0` is uniform with
  the FIN slot; `q = 1..15` is the truncated geometric law with the published
  fitted lambdas, half-to-even rounding, the bucket clamp and top-down pass.
  `ScBcpackModelCum` exposes the tables to the harness.
- **Scheduler.** The round-based, lane-number-priority scheduler: at most 8
  byte requests per round, a stalled run lane does not decode that round, a
  merge costs a round, zero restoration is free, unit synchronisation through
  `pending`, `20*units + 200` round limit.
- **BC1 assembly.** Folded back-reference distance (window 255, history
  entries for virtual blocks −1…−255), per-channel zigzag deltas modulo
  `{32,64,32,32,64,32}`, colour words, and the four 2-bit selector quadrants
  mapped through the table.
- **Strictness / fail closed.** Every record must end with its state back at
  `L` exactly, every lane must carry exactly `outBytes/8` symbols, no record
  may have unread bytes, and the chunk must be fully consumed. On any
  rejection the destination buffer is not written.

Encoder (harness only, `tests/bcpack_b1.cpp`): the inverse rANS encoder plus
`ScBcpackBuildStream`, which runs the *same* scheduler with per-lane byte
queues to learn the order in which the decoder will fetch bytes. There is one
scheduler implementation; the encoder drives it from queues instead of a
stream.

## BC1 tables

`ScBc1Tables` holds the two learned tables (255 history endpoint pairs and the
256-entry quadrant permutation). B1 ships **self-derived** tables
(`ScBc1TablesDefault`: all-zero history, identity quadrant mapping) so the
learn-tables path can be proven end to end. They are not the constants fitted
from real game chunks (those are not published in either reference), so this
module does **not** decode real game chunks without those tables. What is
proven here is the mechanism, not the corpus.

Learn-tables in the harness: 255 blocks are generated so that block `i`
references history entry `255 - i`, in two independent passes with different
deltas. From the block bytes and the encoder's symbols alone, the harness
recovers every history entry and every quadrant code, requires both passes to
agree, and compares the recovered tables with the decoder's. Result: 255/255
history entries and 256/256 quadrant codes recovered, identical.

## B2 — entropy-only lane layouts (BC3, BC4, BC5, BC6H, BC7)

`ScBcpackCodecLayout` exposes the per-codec lane table and
`ScBcpackDecodeLanes` runs the same records, models and scheduler without any
block semantics, returning the lane symbols (lane after lane, matching the
reference's `lanes` output):

| codec | lanes | base widths | units/block | block bytes |
| --- | --- | --- | --- | --- |
| BC1 | 12 | 8,8,5,6,5,5,6,5,8,8,8,8 | 1 | 8 |
| BC3 | 12 | 4,4,8,6,6,6,6,5,6,5,8,8 | 2 | 16 |
| BC4 | 12 | 8,8,8,8,6,6,6,6,6,6,6,6 | 1 | 8 |
| BC5 | 12 | 4,4,8,8,6,6,6,6,6,6,6,6 | 2 | 16 |
| BC6H | 16 | 4,4,8,8,8,5,5,5,6,6,6,8,8,8,8,5 | 2 | 16 |
| BC7 | 16 | 4,4,8,8,8,8,8,7,7,5,6,8,8,8,8,5 | 2 | 16 |

BC7's lane 15 is 5 bits wide (reading it as 8 desynchronises the lane). BC2 has
no published lane layout in either source and is refused with
`SC_BCPACK_UNSUPPORTED_CODEC`.

Proof in the harness: for every codec, random symbols → our encoder → our
scheduler → our lane decoder reproduce the same symbols at 1, 16, 64 and 256
blocks (in coding units: BC3/BC5/BC6H/BC7 carry two units per block). One
third of the lanes use a width reduction so that path is covered too.

Cross-check: `lanes --codec <c> --size N` from the independent reference over
the same chunks — **24/24 codec/block comparisons identical** (6 codecs × 4
block counts), in addition to the B1 checks.

## Cross-check against an independent implementation

`bcpack-decoder` (`C:\Users\seths\Downloads\bcpack-decoder-reference`, MIT,
built in a temporary copy; no code copied into this tree) was used as the second
implementation:

```
bcpack_b1.exe --cross <bcpack-decoder.exe> <workdir>
```

- **Probability tables:** our `ScBcpackModelCum` against `model --q --width`
  for all 16 × 8 combinations — **128/128 identical**.
- **BC1 chunks:** our encoder's chunks at 1, 16, 64 and 256 blocks are decoded
  by the reference with our table file; blocks are compared byte for byte, and
  the reference's `lanes` output is compared with our lane symbols —
  **8/8 comparisons identical**.

The first cross-check run disagreed on 71/128 tables and every chunk. The cause
was in our model builder (the last bucket's boundary was overwritten by the
table end instead of being computed from the fitted law); once corrected, both
comparisons agree exactly. The harness prints the input and both outputs on any
disagreement rather than hiding it.

Note on corruption: BCPack has no checksum. In the harness, 13/16 single-byte
corruptions are rejected (destination untouched); 3 form another valid stream
with the state still returning to `L`. That is a property of the format, and it
is reported, not papered over.

## Chunk-table storage field (detection only)

`.ttpf` chunk-table entry bits 26..29 say how the **next** chunk is stored:
0 raw, 1..7 BCPack BC1..BC7, 8 zlib (both sources in the merged spec agree;
the separate "codec id = format × 4" note is unresolved and does not match
this field). `ScBcpackChunkStorage` extracts it and
`ScBcpackChunkStorageName` names it; values 9..15 are reported as unknown.
**No wiring**: nothing decompresses, dispatches or schedules on this value yet.

## Open / undetermined

- The real BC1 learned tables (history and quadrant constants) are data-derived
  and not published, so real game chunks stay undecodable until they are
  calibrated from own data (B3 territory). The mechanism is complete and
  independently verified.
- Non-flushed records (flush bit clear) are supported in the decoder but were
  never observed in real data and their interaction with the scheduler timing
  is unconfirmed — the harness only produces flushed records.
- BC2–BC7 lane layouts, entropy-only support and block remapping are later
  milestones (B2–B5).
- The 255-entry history cannot be exhausted by a single byte lane (max distance
  255), so "unknown history entry" is unreachable with the full table; the
  decoder still rejects it defensively.
