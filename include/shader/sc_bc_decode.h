// ScarlettCompat shader pipeline - BCn block decode (BC1, BC2, BC3, BC4, BC5, BC6H, BC7).
// MIT. New file; not wired into the boot path.
//
// WHAT THIS IS
//   A CPU decoder from compressed BCn blocks to texels, and a helper that walks a whole
//   image in block row-major order. It is the piece the texture path needs once a stream
//   has been decompressed to BCn blocks (DirectStorage/BCPack order is DEFLATE -> BCPack
//   -> swizzle; this module is the "BCn block -> texels" half).
//
// WHAT IS VERIFIED, AND AGAINST WHAT
//   BC1..BC5 and BC7 are verified byte-for-byte against an independent decoder - the
//   texture2ddecoder Python package, a separate implementation - on generated blocks for
//   every format, with all eight BPTC modes forced, plus analytic cases where the
//   expected texels follow from the block layout. BC1/BC2/BC3 colour is implemented here
//   from the format because bcdec interpolates in 5:6:5 space and lands one unit off the
//   D3D decode; BC4, BC5 and BC7 come from bcdec (third_party/bcdec.h, Unlicense),
//   vendored unmodified. See docs/BC7-VERIFICATION.md for the evidence, the three places
//   the reference itself is wrong, and what is still open.
//   Hardware cross-check: tests/shader/sc_bc_gpu_verify.cpp decodes the same blocks on a
//   real D3D12 GPU. BC7 and BC6H come back bit-exact there; BC1..BC5 differ by at most two
//   8-bit units in interpolated texels, because the hardware's interpolation is not
//   reproduced exactly by any integer rule (both software decoders agree with each other
//   exactly). Alpha is exact everywhere, including BC1's 1-bit punch-through.
//   Nothing here has decoded a real game texture yet: no captured BC7 texture exists in
//   this tree, and the BCPack entropy layer that would feed it is still E_NOTIMPL.
//
// NOT VERIFIED
//   BC6H (HDR) is decoded and structurally checked only. The installed reference tone-maps
//   it to RGBA8, so no independent HDR oracle exists here; a GPU float readback or a real
//   HDR asset is what would close it. BCPack itself (the rANS layer) is not implemented:
//   its bitstream and history tables are not public, so the honest state is "BCn blocks
//   decode, the stream around them does not yet".
#pragma once

#include <cstddef>
#include <cstdint>

namespace sc {

enum class sc_bc_format : uint32_t {
    bc1 = 1, // DXT1 / BC1, with the 1-bit punch-through alpha variant
    bc2 = 2, // DXT3: BC1 colour + explicit 4-bit alpha
    bc3 = 3, // DXT5: BC1 colour + interpolated alpha
    bc4 = 4, // single channel (ATI1N), unsigned
    bc5 = 5, // two channels (ATI2N), unsigned
    bc6h = 6, // HDR RGB half float, unsigned and signed variants
    bc7 = 7, // BPTC, all eight modes
};

// Bytes per 4x4 block: 8 for BC1 and BC4, 16 for the rest (BC2, BC3, BC5, BC6H, BC7).
uint32_t sc_bc_block_bytes(sc_bc_format format);
const char* sc_bc_format_name(sc_bc_format format);

// Decodes one block into 16 RGBA8 texels, row-major (4 rows of 4 pixels, 64 bytes).
// `block_size` must equal sc_bc_block_bytes(format); returns false for an unknown
// format, a null pointer or a wrong block size, and writes nothing in that case.
bool sc_bc_decode_block(sc_bc_format format, const uint8_t* block, size_t block_size,
                        uint8_t* rgba_out);

// BC6H is HDR: it has no RGBA8 form, so it has its own entry point. One block decodes
// to 16 RGB float32 texels, row-major (4 rows of 4 pixels, 48 floats, no alpha).
// `is_signed` selects the signed variant of the format.
bool sc_bc_decode_block_bc6h(const uint8_t* block, size_t block_size, float* rgb_out,
                             bool is_signed);

// Image form of the same: `dst_stride_floats` is the row stride in floats and must be
// at least width * 3. Width and height must be positive multiples of 4.
bool sc_bc_decode_image_bc6h(uint32_t width, uint32_t height, const uint8_t* src,
                             size_t src_size, float* dst, size_t dst_stride_floats,
                             bool is_signed);

// Decodes `width` x `height` texels starting at `src`. Width and height must be
// positive multiples of 4; `src_size` must cover every block; `dst_stride` must be at
// least width * 4 bytes. Blocks are read row-major, block rows top to bottom, which is
// the order a BCn stream is stored in before the console's tiling is applied.
// BC6H is refused here (it has no RGBA8 form); use sc_bc_decode_image_bc6h.
bool sc_bc_decode_image(sc_bc_format format, uint32_t width, uint32_t height,
                        const uint8_t* src, size_t src_size, uint8_t* dst,
                        size_t dst_stride);

} // namespace sc
