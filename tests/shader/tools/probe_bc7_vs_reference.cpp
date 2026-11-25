#include <cstdio>
#include <cstring>
#include <cstdint>
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "../../third_party/bcdec.h"

static void show(const char* name, const uint8_t* block, int mode) {
    uint8_t px[4 * 4 * 4] = {};
    if (mode == 1) bcdec_bc1(block, px, 4 * 4);
    else bcdec_bc7(block, px, 4 * 4);
    std::printf("%s p0=(%u,%u,%u,%u) p5=(%u,%u,%u,%u)\n", name, px[0], px[1], px[2], px[3],
                px[20], px[21], px[22], px[23]);
}

int main() {
    uint8_t bc1[8] = {0x00, 0xf8, 0x1f, 0x00, 0, 0, 0, 0}; // c0 red 565, c1 blue, idx 0
    show("bc1_red", bc1, 1);
    uint8_t zero[16] = {};
    show("bc7_zero", zero, 7);
    uint8_t mode1[16] = {};
    mode1[0] = 0x02; // mode 1 (bit 1 set), rest zero
    show("bc7_mode1_zero", mode1, 7);
    uint8_t mode6[16] = {};
    mode6[0] = 0x40; // mode 6 (bit 6 set)
    show("bc7_mode6_zero", mode6, 7);
    return 0;
}
