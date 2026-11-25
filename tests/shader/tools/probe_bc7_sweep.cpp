#include <cstdio>
#include <cstdint>
#include <vector>
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "../../third_party/bcdec.h"

int main() {
    std::FILE* f = std::fopen("out/bcref/blocks.bin", "rb");
    if (!f) return 1;
    std::vector<uint8_t> blocks;
    uint8_t chunk[16];
    while (std::fread(chunk, 1, 16, f) == 16) blocks.insert(blocks.end(), chunk, chunk + 16);
    std::fclose(f);
    std::FILE* o = std::fopen("out/bcref/decoded.bin", "wb");
    if (!o) return 2;
    for (size_t i = 0; i + 16 <= blocks.size(); i += 16) {
        uint8_t px[64] = {};
        bcdec_bc7(&blocks[i], px, 16);
        std::fwrite(px, 1, 64, o);
    }
    std::fclose(o);
    std::printf("decoded %zu blocks\n", blocks.size() / 16);
    return 0;
}
