/* SHA-1 (ROM identity, MODDING.md §2.1) and SHA-256 (pack.lock, manifest and
 * content hashes, §3.5) for the resource layer.  Straight from FIPS 180-4;
 * byte-at-a-time, which is plenty for 16 MiB once per extraction. */

#include <string.h>

#include "res_hash.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void Sha1Block(u32 h[5], const u8 *p)
{
    u32 w[80], a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        u32 f, k;

        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static const u32 kK256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void Sha256Block(u32 h[8], const u8 *p)
{
    u32 w[64], s[8], t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 64; i++)
        w[i] = (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7]
             + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];
    memcpy(s, h, sizeof s);
    for (i = 0; i < 64; i++) {
        t1 = s[7] + (ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6]))
           + kK256[i] + w[i];
        t2 = (ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1;
        s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++)
        h[i] += s[i];
}

/* Merkle-Damgard padding shared by both: 64-byte blocks, big-endian length. */
static void Digest(const void *data, size_t len, u32 *h, int words,
                   void (*block)(u32 *, const u8 *), u8 *out)
{
    const u8 *p = (const u8 *)data;
    u8 tail[128];
    size_t full = len / 64 * 64, rest = len - full, i, n;
    unsigned long long bits = (unsigned long long)len * 8;

    for (i = 0; i < full; i += 64)
        block(h, p + i);
    memset(tail, 0, sizeof tail);
    memcpy(tail, p + full, rest);
    tail[rest] = 0x80;
    n = rest + 1 + 8 <= 64 ? 64 : 128;
    for (i = 0; i < 8; i++)
        tail[n - 1 - i] = (u8)(bits >> (8 * i));
    for (i = 0; i < n; i += 64)
        block(h, tail + i);
    for (i = 0; i < (size_t)words; i++) {
        out[4 * i] = (u8)(h[i] >> 24);
        out[4 * i + 1] = (u8)(h[i] >> 16);
        out[4 * i + 2] = (u8)(h[i] >> 8);
        out[4 * i + 3] = (u8)h[i];
    }
}

void ResSha1(const void *data, size_t len, u8 out[20])
{
    u32 h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

    Digest(data, len, h, 5, Sha1Block, out);
}

void ResSha256(const void *data, size_t len, u8 out[32])
{
    u32 h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };

    Digest(data, len, h, 8, Sha256Block, out);
}

void ResHex(const u8 *d, size_t n, char *out)
{
    static const char kHex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[2 * i] = kHex[d[i] >> 4];
        out[2 * i + 1] = kHex[d[i] & 15];
    }
    out[2 * n] = '\0';
}

#ifdef __cplusplus
}
#endif
