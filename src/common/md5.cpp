// Self-contained MD5 implementation (RFC 1321). Public-domain style reference
// implementation, adapted to a single translation unit.

#include "md5.h"

#include <cstdint>
#include <cstring>
#include <cstddef>

namespace {

struct MD5Ctx {
    uint32_t state[4];   // ABCDEFGH
    uint32_t count[2];   // number of bits, low and high
    unsigned char buffer[64];
};

static inline uint32_t rotl(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

static inline uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
static inline uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
static inline uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static inline uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }

static inline uint32_t FF(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, int s, uint32_t ac) {
    a += F(b, c, d) + x + ac; a = rotl(a, s) + b; return a;
}
static inline uint32_t GG(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, int s, uint32_t ac) {
    a += G(b, c, d) + x + ac; a = rotl(a, s) + b; return a;
}
static inline uint32_t HH(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, int s, uint32_t ac) {
    a += H(b, c, d) + x + ac; a = rotl(a, s) + b; return a;
}
static inline uint32_t II(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, int s, uint32_t ac) {
    a += I(b, c, d) + x + ac; a = rotl(a, s) + b; return a;
}

static void md5_encode(unsigned char* out, const uint32_t* in, unsigned int len) {
    // `len` is the length in BYTES of the output buffer.
    for (unsigned int i = 0, j = 0; j < len; ++i, j += 4) {
        out[j]     = (unsigned char)(in[i] & 0xff);
        out[j + 1] = (unsigned char)((in[i] >> 8) & 0xff);
        out[j + 2] = (unsigned char)((in[i] >> 16) & 0xff);
        out[j + 3] = (unsigned char)((in[i] >> 24) & 0xff);
    }
}

static void md5_decode(uint32_t* out, const unsigned char* in, unsigned int len) {
    for (unsigned int i = 0, j = 0; i < len; ++i, j += 4)
        out[i] = ((uint32_t)in[j]) | (((uint32_t)in[j + 1]) << 8) |
                 (((uint32_t)in[j + 2]) << 16) | (((uint32_t)in[j + 3]) << 24);
}

static unsigned char PADDING[64] = {
    0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void md5_transform(uint32_t state[4], const unsigned char block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[16];
    md5_decode(x, block, 16);

    a = FF(a, b, c, d, x[0],  7, 0xd76aa478); d = FF(d, a, b, c, x[1], 12, 0xe8c7b756);
    c = FF(c, d, a, b, x[2], 17, 0x242070db); b = FF(b, c, d, a, x[3], 22, 0xc1bdceee);
    a = FF(a, b, c, d, x[4],  7, 0xf57c0faf); d = FF(d, a, b, c, x[5], 12, 0x4787c62a);
    c = FF(c, d, a, b, x[6], 17, 0xa8304613); b = FF(b, c, d, a, x[7], 22, 0xfd469501);
    a = FF(a, b, c, d, x[8],  7, 0x698098d8); d = FF(d, a, b, c, x[9], 12, 0x8b44f7af);
    c = FF(c, d, a, b, x[10],17, 0xffff5bb1); b = FF(b, c, d, a, x[11],22, 0x895cd7be);
    a = FF(a, b, c, d, x[12], 7, 0x6b901122); d = FF(d, a, b, c, x[13],12, 0xfd987193);
    c = FF(c, d, a, b, x[14],17, 0xa679438e); b = FF(b, c, d, a, x[15],22, 0x49b40821);

    a = GG(a, b, c, d, x[1],  5, 0xf61e2562); d = GG(d, a, b, c, x[6],  9, 0xc040b340);
    c = GG(c, d, a, b, x[11],14, 0x265e5a51); b = GG(b, c, d, a, x[0], 20, 0xe9b6c7aa);
    a = GG(a, b, c, d, x[5],  5, 0xd62f105d); d = GG(d, a, b, c, x[10], 9, 0x02441453);
    c = GG(c, d, a, b, x[15],14, 0xd8a1e681); b = GG(b, c, d, a, x[4], 20, 0xe7d3fbc8);
    a = GG(a, b, c, d, x[9],  5, 0x21e1cde6); d = GG(d, a, b, c, x[14], 9, 0xc33707d6);
    c = GG(c, d, a, b, x[3], 14, 0xf4d50d87); b = GG(b, c, d, a, x[8], 20, 0x455a14ed);
    a = GG(a, b, c, d, x[13], 5, 0xa9e3e905); d = GG(d, a, b, c, x[2],  9, 0xfcefa3f8);
    c = GG(c, d, a, b, x[7], 14, 0x676f02d9); b = GG(b, c, d, a, x[12],20, 0x8d2a4c8a);

    a = HH(a, b, c, d, x[5],  4, 0xfffa3942); d = HH(d, a, b, c, x[8], 11, 0x8771f681);
    c = HH(c, d, a, b, x[11],16, 0x6d9d6122); b = HH(b, c, d, a, x[14],23, 0xfde5380c);
    a = HH(a, b, c, d, x[1],  4, 0xa4beea44); d = HH(d, a, b, c, x[4], 11, 0x4bdecfa9);
    c = HH(c, d, a, b, x[7], 16, 0xf6bb4b60); b = HH(b, c, d, a, x[10],23, 0xbebfbc70);
    a = HH(a, b, c, d, x[13], 4, 0x289b7ec6); d = HH(d, a, b, c, x[0], 11, 0xeaa127fa);
    c = HH(c, d, a, b, x[3], 16, 0xd4ef3085); b = HH(b, c, d, a, x[6], 23, 0x04881d05);
    a = HH(a, b, c, d, x[9],  4, 0xd9d4d039); d = HH(d, a, b, c, x[12],11, 0xe6db99e5);
    c = HH(c, d, a, b, x[15],16, 0x1fa27cf8); b = HH(b, c, d, a, x[2], 23, 0xc4ac5665);

    a = II(a, b, c, d, x[0],  6, 0xf4292244); d = II(d, a, b, c, x[7], 10, 0x432aff97);
    c = II(c, d, a, b, x[14],15, 0xab9423a7); b = II(b, c, d, a, x[5], 21, 0xfc93a039);
    a = II(a, b, c, d, x[12], 6, 0x655b59c3); d = II(d, a, b, c, x[3], 10, 0x8f0ccc92);
    c = II(c, d, a, b, x[10],15, 0xffeff47d); b = II(b, c, d, a, x[1], 21, 0x85845dd1);
    a = II(a, b, c, d, x[8],  6, 0x6fa87e4f); d = II(d, a, b, c, x[15],10, 0xfe2ce6e0);
    c = II(c, d, a, b, x[6], 15, 0xa3014314); b = II(b, c, d, a, x[13],21, 0x4e0811a1);
    a = II(a, b, c, d, x[4],  6, 0xf7537e82); d = II(d, a, b, c, x[11],10, 0xbd3af235);
    c = II(c, d, a, b, x[2], 15, 0x2ad7d2bb); b = II(b, c, d, a, x[9], 21, 0xeb86d391);

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static void md5_init(MD5Ctx* c) {
    c->state[0] = 0x67452301;
    c->state[1] = 0xefcdab89;
    c->state[2] = 0x98badcfe;
    c->state[3] = 0x10325476;
    c->count[0] = c->count[1] = 0;
}

static void md5_update(MD5Ctx* c, const unsigned char* input, unsigned int len) {
    unsigned int i, idx, partLen;
    idx = (unsigned int)((c->count[0] >> 3) & 0x3f);
    if ((c->count[0] += ((uint32_t)len << 3)) < ((uint32_t)len << 3)) c->count[1]++;
    c->count[1] += ((uint32_t)len >> 29);
    partLen = 64 - idx;
    if (len >= partLen) {
        memcpy(c->buffer + idx, input, partLen);
        md5_transform(c->state, c->buffer);
        for (i = partLen; i + 63 < len; i += 64) md5_transform(c->state, input + i);
        idx = 0;
    } else {
        i = 0;
    }
    memcpy(c->buffer + idx, input + i, len - i);
}

static void md5_final(unsigned char digest[16], MD5Ctx* c) {
    unsigned char bits[8];
    md5_encode(bits, c->count, 8);
    unsigned int idx = (unsigned int)((c->count[0] >> 3) & 0x3f);
    unsigned int padLen = (idx < 56) ? (56 - idx) : (120 - idx);
    md5_update(c, PADDING, padLen);
    md5_update(c, bits, 8);
    md5_encode(digest, c->state, 16);
}

}  // namespace

std::string md5Hex(const std::string& input) {
    MD5Ctx ctx;
    md5_init(&ctx);
    md5_update(&ctx, (const unsigned char*)input.data(), (unsigned int)input.size());
    unsigned char digest[16];
    md5_final(digest, &ctx);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.resize(32);
    for (int i = 0; i < 16; ++i) {
        out[i * 2]     = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    return out;
}
