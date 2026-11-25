#include <cstdio>
#include <cstdint>
#include <vector>
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "../../third_party/bcdec.h"

int main() {
    std::FILE* f = std::fopen("out/bcref/blocks6.bin", "rb");
    if (!f) return 1;
    std::vector<uint8_t> blocks;
    uint8_t chunk[16];
    while (std::fread(chunk, 1, 16, f) == 16) blocks.insert(blocks.end(), chunk, chunk + 16);
    std::fclose(f);
    std::FILE* o = std::fopen("out/bcref/decoded6.bin", "wb");
    if (!o) return 2;
    for (size_t i = 0; i + 16 <= blocks.size(); i += 16) {
        float px[16 * 3] = {};
        bcdec_bc6h_float(&blocks[i], px, 4 * 3, 0);
        std::fwrite(px, 1, sizeof(px), o);
    }
    std::fclose(o);
    std::printf("decoded %zu bc6h blocks\n", blocks.size() / 16);
    return 0;
}
