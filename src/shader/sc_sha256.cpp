// ScarlettCompat shader pipeline - content hashing (MIT, clean-room).
#include "shader/sc_sha256.h"

#include <cstring>

namespace sc {
namespace {

const uint32_t kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

inline uint32_t load_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void store_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

struct sha_ctx {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t buflen;
};

void sha_init(sha_ctx& c) {
    static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::memcpy(c.h, iv, sizeof(iv));
    c.total = 0;
    c.buflen = 0;
}

void sha_block(sha_ctx& c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = load_be32(p + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    uint32_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
        const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }

    c.h[0] += a;
    c.h[1] += b;
    c.h[2] += cc;
    c.h[3] += d;
    c.h[4] += e;
    c.h[5] += f;
    c.h[6] += g;
    c.h[7] += h;
}

void sha_update(sha_ctx& c, const uint8_t* p, size_t n) {
    c.total += n;
    if (c.buflen != 0) {
        const size_t take = (n < (64 - c.buflen)) ? n : (64 - c.buflen);
        std::memcpy(c.buf + c.buflen, p, take);
        c.buflen += take;
        p += take;
        n -= take;
        if (c.buflen == 64) {
            sha_block(c, c.buf);
            c.buflen = 0;
        }
    }
    while (n >= 64) {
        sha_block(c, p);
        p += 64;
        n -= 64;
    }
    if (n != 0) {
        std::memcpy(c.buf, p, n);
        c.buflen = n;
    }
}

void sha_final(sha_ctx& c, uint8_t out[32]) {
    const uint64_t bits = c.total * 8ull;
    uint8_t pad[72];
    size_t padlen = 0;
    pad[padlen++] = 0x80;
    const size_t rem = static_cast<size_t>(c.total % 64);
    size_t zeros = (rem < 56) ? (55 - rem) : (119 - rem);
    while (zeros-- > 0) {
        pad[padlen++] = 0x00;
    }
    store_be32(pad + padlen, static_cast<uint32_t>(bits >> 32));
    store_be32(pad + padlen + 4, static_cast<uint32_t>(bits & 0xffffffffu));
    padlen += 8;
    sha_update(c, pad, padlen);
    for (int i = 0; i < 8; ++i) {
        store_be32(out + i * 4, c.h[i]);
    }
}

} // namespace

sha256_digest sc_sha256(const void* data, size_t size) {
    sha_ctx c;
    sha_init(c);
    if (data != nullptr && size != 0) {
        sha_update(c, static_cast<const uint8_t*>(data), size);
    }
    sha256_digest d;
    sha_final(c, d.bytes);
    return d;
}

std::string sc_hex(const void* data, size_t size) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    if (p == nullptr) {
        return out;
    }
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[p[i] >> 4]);
        out.push_back(kDigits[p[i] & 0x0f]);
    }
    return out;
}

std::string sc_hex(const sha256_digest& d) { return sc_hex(d.bytes, sizeof(d.bytes)); }

std::string sc_sha256_hex(const void* data, size_t size) { return sc_hex(sc_sha256(data, size)); }

uint64_t sc_fnv1a64(const void* data, size_t size, uint64_t seed) {
    uint64_t h = seed;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    if (p == nullptr) {
        return h;
    }
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 0x100000001b3ull;
    }
    return h;
}

void sc_fnv1a64_stream::add(const void* data, size_t size) {
    value_ = sc_fnv1a64(data, size, value_);
}

void sc_fnv1a64_stream::add_u32(uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                    static_cast<uint8_t>(v >> 24)};
    add(b, sizeof(b));
}

void sc_fnv1a64_stream::add_u64(uint64_t v) {
    add_u32(static_cast<uint32_t>(v & 0xffffffffull));
    add_u32(static_cast<uint32_t>(v >> 32));
}

} // namespace sc
