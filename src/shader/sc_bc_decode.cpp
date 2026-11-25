// ScarlettCompat shader pipeline - BCn block decode implementation.
// MIT. New file; not wired into the boot path.
//
// WHERE THE CODE COMES FROM
//   BC4, BC5 and BC7 come from bcdec (third_party/bcdec.h, Unlicense/public domain),
//   vendored unmodified. BC1, BC2 and BC3 are written here, because bcdec interpolates
//   the 5:6:5 endpoints in 5:6:5 space and then expands, which lands one bit off the
//   D3D decode: the format expands the endpoints to 8 bits first (bit replication) and
//   interpolates those 8-bit values. That difference is visible - (24,99,92) vs
//   (24,100,93) on a real block - and the reference decoder this project verifies
//   against follows the 8-bit rule, as does the hardware. Rather than patch the
//   vendored header, the 565 colour path lives here, written from the format: endpoints
//   expanded by bit replication, then floor((2a+b)/3) and floor((a+2b)/3) for the
//   four-colour mode, floor((a+b)/2) and transparent black for the three-colour mode.
#include "shader/sc_bc_decode.h"

#include <cstdint>

// bcdec is third-party, vendored unmodified (third_party/bcdec.h, Unlicense). Its own
// code trips MSVC level-4 diagnostics that this project builds with /W4 /WX, so the
// warnings are scoped to this include and nowhere else: the suppression covers vendored
// code only, never anything written here.
#pragma warning(push)
#pragma warning(disable : 4244 4245 4305 4389 4505 4701 4702)
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "bcdec.h"
#pragma warning(pop)

namespace sc {
namespace {

constexpr uint32_t kBlockPixels = 16;
constexpr uint32_t kBlockSide = 4;

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Expands a 5:6:5 endpoint to 8 bits the way the format defines it: replicate the high
// bits, which is the same as rounding v * 255 / max.
void expand_565(uint16_t colour, uint8_t rgb[3]) {
    const uint32_t r = (colour >> 11) & 0x1Fu;
    const uint32_t g = (colour >> 5) & 0x3Fu;
    const uint32_t b = colour & 0x1Fu;
    rgb[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
    rgb[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
    rgb[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
}

// Decodes the 8-byte 565 colour block shared by BC1, BC2 and BC3 into 16 RGB texels.
// `four_colour_only` is set for BC2/BC3, whose colour block is always the four-colour
// mode whatever the endpoint order says; BC1 uses the endpoint order to pick the
// three-colour mode and reports it through `punch_through`.
void decode_colour_block(const uint8_t* block, bool four_colour_only, uint8_t* rgb_out,
                         bool* punch_through) {
    const uint16_t c0 = read_u16(block);
    const uint16_t c1 = read_u16(block + 2);
    uint8_t palette[4][3] = {};
    expand_565(c0, palette[0]);
    expand_565(c1, palette[1]);

    const bool punch = !four_colour_only && c0 <= c1;
    if (punch) {
        for (uint32_t i = 0; i < 3; ++i) {
            palette[2][i] = static_cast<uint8_t>((palette[0][i] + palette[1][i]) / 2);
            palette[3][i] = 0;
        }
    } else {
        for (uint32_t i = 0; i < 3; ++i) {
            palette[2][i] =
                static_cast<uint8_t>((2 * palette[0][i] + palette[1][i]) / 3);
            palette[3][i] =
                static_cast<uint8_t>((palette[0][i] + 2 * palette[1][i]) / 3);
        }
    }

    const uint32_t indices = read_u32(block + 4);
    for (uint32_t i = 0; i < kBlockPixels; ++i) {
        const uint32_t index = (indices >> (2 * i)) & 0x3u;
        rgb_out[i * 3 + 0] = palette[index][0];
        rgb_out[i * 3 + 1] = palette[index][1];
        rgb_out[i * 3 + 2] = palette[index][2];
    }
    if (punch_through != nullptr) {
        *punch_through = punch;
    }
}

void write_rgba(uint8_t* rgba_out, const uint8_t* rgb, const uint8_t* alpha) {
    for (uint32_t i = 0; i < kBlockPixels; ++i) {
        rgba_out[i * 4 + 0] = rgb[i * 3 + 0];
        rgba_out[i * 4 + 1] = rgb[i * 3 + 1];
        rgba_out[i * 4 + 2] = rgb[i * 3 + 2];
        rgba_out[i * 4 + 3] = alpha[i];
    }
}

// BC4/BC5 store one or two channels; bcdec writes them with `destinationPitch` bytes
// per row, so each row is written at the natural channel stride.
void expand_channels(const uint8_t* src, uint32_t channels, uint8_t* rgba_out) {
    for (uint32_t i = 0; i < kBlockPixels; ++i) {
        rgba_out[i * 4 + 0] = src[i * channels];
        rgba_out[i * 4 + 1] = channels > 1 ? src[i * channels + 1] : 0;
        rgba_out[i * 4 + 2] = 0;
        rgba_out[i * 4 + 3] = 255;
    }
}

} // namespace

uint32_t sc_bc_block_bytes(sc_bc_format format) {
    switch (format) {
    case sc_bc_format::bc1:
    case sc_bc_format::bc4:
        return 8;
    case sc_bc_format::bc2:
    case sc_bc_format::bc3:
    case sc_bc_format::bc5:
    case sc_bc_format::bc6h:
    case sc_bc_format::bc7:
        return 16;
    default:
        return 0;
    }
}

const char* sc_bc_format_name(sc_bc_format format) {
    switch (format) {
    case sc_bc_format::bc1:
        return "BC1";
    case sc_bc_format::bc2:
        return "BC2";
    case sc_bc_format::bc3:
        return "BC3";
    case sc_bc_format::bc4:
        return "BC4";
    case sc_bc_format::bc5:
        return "BC5";
    case sc_bc_format::bc6h:
        return "BC6H";
    case sc_bc_format::bc7:
        return "BC7";
    default:
        return "unknown";
    }
}

bool sc_bc_decode_block(sc_bc_format format, const uint8_t* block, size_t block_size,
                        uint8_t* rgba_out) {
    if (block == nullptr || rgba_out == nullptr) {
        return false;
    }
    const uint32_t bytes = sc_bc_block_bytes(format);
    if (bytes == 0 || block_size != bytes) {
        return false;
    }
    switch (format) {
    case sc_bc_format::bc1: {
        uint8_t rgb[kBlockPixels * 3] = {};
        uint8_t alpha[kBlockPixels] = {};
        bool punch_through = false;
        decode_colour_block(block, false, rgb, &punch_through);
        const uint32_t indices = read_u32(block + 4);
        for (uint32_t i = 0; i < kBlockPixels; ++i) {
            // Three-colour mode makes index 3 transparent black, the format's 1-bit
            // alpha; the four-colour mode is opaque everywhere.
            const bool transparent =
                punch_through && ((indices >> (2 * i)) & 0x3u) == 0x3u;
            alpha[i] = transparent ? 0 : 255;
        }
        write_rgba(rgba_out, rgb, alpha);
        return true;
    }
    case sc_bc_format::bc2: {
        uint8_t rgb[kBlockPixels * 3] = {};
        uint8_t alpha[kBlockPixels] = {};
        decode_colour_block(block + 8, true, rgb, nullptr);
        // The alpha half is 16 explicit 4-bit values, low nibble first, replicated to
        // 8 bits (a 4-bit value v becomes v * 17).
        for (uint32_t i = 0; i < kBlockPixels; ++i) {
            const uint8_t packed = block[i / 2];
            const uint8_t nibble =
                (i % 2 == 0) ? static_cast<uint8_t>(packed & 0x0Fu)
                             : static_cast<uint8_t>(packed >> 4);
            alpha[i] = static_cast<uint8_t>(nibble * 17);
        }
        write_rgba(rgba_out, rgb, alpha);
        return true;
    }
    case sc_bc_format::bc3: {
        uint8_t rgb[kBlockPixels * 3] = {};
        decode_colour_block(block + 8, true, rgb, nullptr);
        // The alpha half has the same layout as a BC4 block, so it reuses that decode.
        uint8_t alpha[kBlockPixels] = {};
        bcdec_bc4(block, alpha, static_cast<int>(kBlockSide));
        write_rgba(rgba_out, rgb, alpha);
        return true;
    }
    case sc_bc_format::bc4: {
        uint8_t channel[kBlockPixels] = {};
        bcdec_bc4(block, channel, static_cast<int>(kBlockSide));
        expand_channels(channel, 1, rgba_out);
        return true;
    }
    case sc_bc_format::bc5: {
        uint8_t channels[kBlockPixels * 2] = {};
        bcdec_bc5(block, channels, static_cast<int>(kBlockSide * 2));
        expand_channels(channels, 2, rgba_out);
        return true;
    }
    case sc_bc_format::bc6h:
        // BC6H is HDR (float RGB); use sc_bc_decode_block_bc6h instead.
        return false;
    case sc_bc_format::bc7:
        bcdec_bc7(block, rgba_out, static_cast<int>(kBlockSide * 4));
        return true;
    default:
        return false;
    }
}

bool sc_bc_decode_block_bc6h(const uint8_t* block, size_t block_size, float* rgb_out,
                             bool is_signed) {
    if (block == nullptr || rgb_out == nullptr || block_size != 16) {
        return false;
    }
    bcdec_bc6h_float(block, rgb_out, static_cast<int>(kBlockSide * 3), is_signed ? 1 : 0);
    return true;
}

bool sc_bc_decode_image_bc6h(uint32_t width, uint32_t height, const uint8_t* src,
                             size_t src_size, float* dst, size_t dst_stride_floats,
                             bool is_signed) {
    if (src == nullptr || dst == nullptr || width == 0 || height == 0) {
        return false;
    }
    if (width % kBlockSide != 0 || height % kBlockSide != 0) {
        return false;
    }
    const size_t blocks_x = width / kBlockSide;
    const size_t blocks_y = height / kBlockSide;
    if (blocks_x != 0 && blocks_y > (SIZE_MAX / blocks_x) / 16u) {
        return false;
    }
    if (src_size < blocks_x * blocks_y * 16u) {
        return false;
    }
    if (dst_stride_floats < static_cast<size_t>(width) * 3) {
        return false;
    }

    float rgb[kBlockPixels * 3] = {};
    for (size_t by = 0; by < blocks_y; ++by) {
        for (size_t bx = 0; bx < blocks_x; ++bx) {
            if (!sc_bc_decode_block_bc6h(src + (by * blocks_x + bx) * 16u, 16u, rgb,
                                         is_signed)) {
                return false;
            }
            for (uint32_t row = 0; row < kBlockSide; ++row) {
                float* out = dst + (by * kBlockSide + row) * dst_stride_floats +
                             bx * kBlockSide * 3;
                const float* in = rgb + row * kBlockSide * 3;
                for (uint32_t i = 0; i < kBlockSide * 3; ++i) {
                    out[i] = in[i];
                }
            }
        }
    }
    return true;
}

bool sc_bc_decode_image(sc_bc_format format, uint32_t width, uint32_t height,
                        const uint8_t* src, size_t src_size, uint8_t* dst,
                        size_t dst_stride) {
    if (src == nullptr || dst == nullptr || width == 0 || height == 0) {
        return false;
    }
    if (width % kBlockSide != 0 || height % kBlockSide != 0) {
        return false;
    }
    const uint32_t block_bytes = sc_bc_block_bytes(format);
    if (block_bytes == 0 || format == sc_bc_format::bc6h) {
        // BC6H has no RGBA8 form; its own entry point takes float texels instead.
        return false;
    }
    const size_t blocks_x = width / kBlockSide;
    const size_t blocks_y = height / kBlockSide;
    // Reject sizes whose block count would overflow the size computation before trusting
    // the `src_size` comparison.
    if (blocks_x != 0 && blocks_y > (SIZE_MAX / blocks_x) / block_bytes) {
        return false;
    }
    if (src_size < blocks_x * blocks_y * block_bytes) {
        return false;
    }
    if (dst_stride < static_cast<size_t>(width) * 4) {
        return false;
    }

    uint8_t rgba[kBlockPixels * 4] = {};
    for (size_t by = 0; by < blocks_y; ++by) {
        for (size_t bx = 0; bx < blocks_x; ++bx) {
            const uint8_t* block = src + (by * blocks_x + bx) * block_bytes;
            if (!sc_bc_decode_block(format, block, block_bytes, rgba)) {
                return false;
            }
            for (uint32_t row = 0; row < kBlockSide; ++row) {
                uint8_t* out = dst + (by * kBlockSide + row) * dst_stride +
                               bx * kBlockSide * 4;
                const uint8_t* in = rgba + row * kBlockSide * 4;
                for (uint32_t i = 0; i < kBlockSide * 4; ++i) {
                    out[i] = in[i];
                }
            }
        }
    }
    return true;
}

} // namespace sc
