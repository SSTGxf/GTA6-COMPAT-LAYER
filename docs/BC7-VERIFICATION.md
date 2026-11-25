# BCn block decode: what is verified, and against what

The texture path needs to turn compressed BCn blocks into texels. This is the half of
that job that can be built and checked honestly today: `include/shader/sc_bc_decode.h`
and `src/shader/sc_bc_decode.cpp` decode BC1, BC2, BC3, BC4, BC5, BC6H and BC7 blocks and
whole images. It is not wired into the boot path.

Run the harness:

```
tests\shaderuild_bc_decode_test.bat     # 118 checks, 0 failed, /W4 /WX
tests\shaderuild_bc_gpu_verify.bat      # real GPU decode; needs a D3D12 adapter
tests\shaderuild_all_no_dxc.bat         # gate: 12 sources, C++17 + C++14, zero warnings
tests\shaderuild_link_smoke.bat         # link contract still holds
```

`tests/shader/tools/gen_bc_fixtures.py` regenerates the fixtures from the reference
decoder; `tests/shader/tools/probe_*.cpp` are the small programs used to pin down the
oracle's behaviour. Both are tracked, unlike the throwaway output under `out/`.

## The oracle

Expectations come from an **independent decoder**: the `texture2ddecoder` Python package,
which implements these formats separately from `bcdec`. Its output is BGRA and is
converted to RGBA for the fixtures. The module under test shares no code with it, so
agreement means two implementations agree, not that the code agrees with itself.
`tests/shader/tools/gen_bc_fixtures.py` regenerates `tests/shader/sc_bc_fixtures.h`; `tests/shader/tools/`
also holds the probe programs used to pin down the oracle's behaviour.

## Status per format

| Format | Texels | Verified against the independent decoder | Notes |
| --- | --- | --- | --- |
| BC1 | RGB 4/4 blocks, 16x16 image placement | yes, byte-exact | alpha derived from the format, see below |
| BC2 | RGB 4/4 blocks | yes (colour half is reference BC1 output) | the decoder has no BC2 entry; alpha checked from the nibbles |
| BC3 | RGBA 4/4 blocks | yes, byte-exact | colour half forced four-colour, as the format requires |
| BC4 | RGBA 4/4 blocks | yes, byte-exact | plus an analytic constant block |
| BC5 | RGBA 4/4 blocks | yes, byte-exact | plus an analytic two-channel block |
| BC6H | float RGB, UF16 and SF16 | not against software | the reference tone-maps HDR to RGBA8; verified against **hardware** instead, see below |
| BC7 | RGBA, modes 0-7 forced, 16x16 image | yes, byte-exact | every BPTC mode is exercised |

BC7 was additionally swept over 48 random blocks (six per mode, modes forced through the
mode-bit field) in a one-off probe: all 48 decoded identically to the reference. The
committed harness keeps one block per mode plus a 16x16 image, which is enough to fail if
any mode regresses.

## Why the BC1-BC3 colour path is ours, not bcdec's

`bcdec` interpolates the 5:6:5 endpoints in 5:6:5 space and expands afterwards. The format
expands the endpoints to 8 bits first, by bit replication, and interpolates those 8-bit
values; that is what the hardware does and what the reference decoder does. The two rules
disagree by one unit on real blocks - (24,99,92) versus (24,100,93) - and a fixture caught
it. Rather than patch the vendored header, BC1/BC2/BC3 colour is written in
`sc_bc_decode.cpp` from the format: expand by bit replication, then floor((2a+b)/3) and
floor((a+2b)/3) in the four-colour mode, floor((a+b)/2) for the three-colour mode. With
that rule, 240 random BC1/BC3 blocks match the reference exactly.

BC4, BC5 and BC7 still come from `bcdec` (Unlicense), included only from
`sc_bc_decode.cpp` with the warning scope narrowed to that include.

## Where the oracle is wrong, and what we do instead

Three places where the reference disagrees with the format. Each is documented at its
fixture array, and the harness checks the format rule directly instead:

1. **BC1 three-colour alpha.** The format makes index 3 transparent black; the reference
   returns opaque black. BC1 is therefore an RGB-only oracle, and the harness derives
   `A = 0` for index-3 texels of three-colour blocks itself.
2. **BC3 colour mode.** BC2 and BC3 colour blocks are always the four-colour mode, whatever
   the endpoint order. The reference picks the three-colour palette when colour0 <= colour1.
   BC3 fixtures use four-colour endpoint order, and an analytic case decodes a
   colour0 < colour1 block and asserts the four-colour palette (85,0,0, not 127,0,0).
3. **BC1 whole-image order.** The reference's 16x16 BC1 path disagrees with its own
   per-block BC1 decode. No BC1 image expectation comes from it; the BC7 image is the
   reference-verified image-order check, and BC1 image coverage is placement and stride.

## Hardware cross-check (real GPU)

`tests\shaderuild_bc_gpu_verify.bat` runs `tests/shader/sc_bc_gpu_verify.cpp`: it creates
its own D3D12 compute device, uploads the same fixture blocks as textures, dispatches a
shader that `Load()`s every texel into a float buffer, and compares the readback twice -
against this decoder and against the reference texels.

Measured on an **AMD Radeon RX 5700 XT**. The check needs a D3D12 adapter; on a machine
without one it exits 1 and says so rather than reporting a pass. It is deliberately outside
`build_all_no_dxc.bat`: it links d3d12/dxgi/d3dcompiler and a real adapter, while the gate
must keep proving the decode path links with no D3D at all.

| Format | GPU vs this decoder | GPU vs reference | max deviation | where |
| --- | --- | --- | --- | --- |
| BC1 16x16 | 77/256 texels differ | same 77 | 2 units | colour only |
| BC2 8x8 | 28/64 | 28/64 | 1 unit | colour only, alpha exact |
| BC3 8x8 | 37/64 | 37/64 | 2 units | colour and alpha |
| BC4 8x8 | 35/64 | 35/64 | 2 units | the single channel |
| BC5 8x8 | 34/64 | 34/64 | 2 units | both channels |
| BC7 16x16, modes 0-7 | **0/256** | **0/256** | 0 | bit-exact |
| BC6H_UF16 8x8 | **0/64** | n/a | 0.0 | bit-exact |
| BC6H_SF16 8x8 | **0/64** | n/a | 0.0 | bit-exact |

What that settles:

- **BC6H is verified.** It was the format with no software oracle; the GPU now is one, and
  both signednesses agree exactly, not approximately.
- **BC7 is verified on hardware as well as in software** - all 256 texels of a 16x16 image
  covering all eight modes are bit-identical on the GPU.
- **BC1's 1-bit alpha is real.** All 40 index-3 texels of the three-colour blocks come back
  with alpha 0 and everything else 255, so the rule the reference gets wrong is the rule the
  hardware uses. BC2's alpha nibble order is confirmed the same way (alpha exact on all 64
  texels).
- **BC1-BC5 interpolated texels are not bit-reproducible.** The GPU differs from both
  software decoders by 1-2 units there, and no integer rule reproduces it: on this adapter
  BC4 floor-interpolation matches 29/64 texels and round-to-nearest 42/64, while two
  independent software decoders agree 64/64 with each other. The module therefore keeps the
  rule that both software decoders share (spec-derived, exercised by the fixtures), the GPU
  check allows the measured 2-unit spread, and every difference is printed on each run so
  the numbers cannot drift unnoticed.

## Review pass: what it changed

A read-through of the module and its harness, before the hardware check was wired in,
found these - each fixed rather than noted:

1. `sc_bc_decode_image` accepted `sc_bc_format::bc6h` and only failed deep inside the block
   decoder. It now refuses that format at the top, with the reason, since BC6H has no RGBA8
   form.
2. The image path trusted `blocks_x * blocks_y * block_bytes` for its source-size check
   without guarding against overflow on absurd dimensions. It now rejects sizes whose block
   count cannot be represented before comparing.
3. In the GPU verifier: `&heap_props(...)` took the address of a temporary (C4238), the
   adapter name went through `wcstombs` and the dump through `fopen` (both C4996). Now named
   locals, `WideCharToMultiByte` and `fopen_s`.
4. A duplicated dump block and a stray brace in the verifier were leftovers from editing;
   removed, and both compile paths are warning-free at /W4 again.

Two things the review could not settle from the code alone, and the GPU did settle:

- BC2's alpha nibble order was only "consistent" between the decoder and the fixture
  generator, which is not evidence. Hardware puts the first texel in the low nibble, exactly
  as implemented (alpha exact on all 64 texels).
- BC1's three-colour alpha rule was a format reading. Hardware agrees: 40/40 index-3 texels
  come back transparent.

Left deliberately as is: BC1 reads the 32-bit index word twice (palette, then alpha). It is
one load from the same block, and folding the two would make the alpha rule harder to see.

## What is not verified

- **BC6H against independent software.** It is verified against hardware (exact), not
  against a second software implementation, because the only reference installed here
  tone-maps HDR to RGBA8. Hardware agreement is the stronger of the two checks for the
  purpose this decoder serves, but it is one vendor: an NVIDIA adapter would say whether the
  two GPUs agree with each other.
- **Real game textures.** No captured BC7/BC6H texture exists in this tree. Every block
  here is synthetic, generated with a fixed seed; the fixtures say so.
- **BCPack.** `SC_OPT_DS_DECOMP_BCPACK_BC1..BC7` remain `E_NOTIMPL`. The rANS bitstream and
  the BC7 256-entry history tables are unpublished, so the stream that would feed these
  blocks cannot be implemented or verified. The decoder is the "blocks to texels" half;
  the "stream to blocks" half is still blocked, and no amount of testing here changes that.

So: BC1, BC2, BC3, BC4, BC5 and BC7 decode byte-exactly against an independent
implementation; BC7 and BC6H (signed and unsigned) are additionally bit-exact against real
hardware; BC1-BC5 match hardware within two 8-bit units on interpolated texels, with alpha
exact; and the BCPack layer in front of all of them is still unimplemented.
