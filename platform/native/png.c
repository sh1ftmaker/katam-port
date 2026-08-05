/* A PNG writer, in ninety lines and with no dependency.
 *
 * --screenshot is what makes the native build testable without a person
 * watching it, so it has to work in every build on every platform -- which
 * rules out libpng (a dependency), SDL_image (a separate library that is often
 * not installed) and SDL_SaveBMP (fine, but nothing in a CI log can look at a
 * BMP).
 *
 * A PNG's image data is a zlib stream, and a zlib stream is allowed to consist
 * entirely of *stored* deflate blocks -- a length and then the literal bytes.
 * So the only real work is the two checksums.  The result is about 40% larger
 * than a compressed PNG and is a completely ordinary PNG file otherwise.
 */

#include <stdio.h>
#include <string.h>

#include "native.h"

static u32 Crc32(u32 crc, const u8 *p, size_t n)
{
    static u32 table[256];
    static int built;
    size_t i;

    if (!built) {
        u32 k, j;

        for (k = 0; k < 256; k++) {
            u32 c = k;

            for (j = 0; j < 8; j++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[k] = c;
        }
        built = 1;
    }

    crc ^= 0xFFFFFFFFu;
    for (i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static void Be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);  p[3] = (u8)v;
}

static int Chunk(FILE *f, const char *type, const u8 *data, size_t n)
{
    u8 head[8];
    u8 tail[4];
    u32 crc;

    Be32(head, (u32)n);
    memcpy(head + 4, type, 4);
    crc = Crc32(0, head + 4, 4);
    crc = Crc32(crc, data, n);
    Be32(tail, crc);

    return fwrite(head, 1, 8, f) == 8
        && (n == 0 || fwrite(data, 1, n, f) == n)
        && fwrite(tail, 1, 4, f) == 4;
}

int PortNativeWritePng(const char *path, const u32 *rgba, int w, int h)
{
    static const u8 kSig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    u8 ihdr[13];
    u8 *raw, *z;
    size_t rawLen, zLen, pos, off;
    u32 s1 = 1, s2 = 0;
    FILE *f;
    int y, x, ok;

    if (w <= 0 || h <= 0)
        return 0;

    /* Filter byte 0 (none) in front of every row, then RGB triples.  The
     * framebuffer is 0xAABBGGRR -- see ToRgba in platform/ppu.c -- and always
     * opaque, so the alpha channel is dropped rather than written. */
    rawLen = (size_t)h * (1 + (size_t)w * 3);
    raw = (u8 *)malloc(rawLen);
    if (raw == NULL)
        return 0;

    pos = 0;
    for (y = 0; y < h; y++) {
        raw[pos++] = 0;
        for (x = 0; x < w; x++) {
            u32 c = rgba[(size_t)y * w + x];

            raw[pos++] = (u8)(c & 0xFF);
            raw[pos++] = (u8)((c >> 8) & 0xFF);
            raw[pos++] = (u8)((c >> 16) & 0xFF);
        }
    }

    for (pos = 0; pos < rawLen; pos++) {
        s1 = (s1 + raw[pos]) % 65521;
        s2 = (s2 + s1) % 65521;
    }

    /* zlib header, then stored deflate blocks of at most 65535 bytes, then the
     * Adler-32 of the uncompressed data. */
    zLen = 2 + 4 + rawLen + 5 * ((rawLen + 65534) / 65535);
    z = (u8 *)malloc(zLen);
    if (z == NULL) {
        free(raw);
        return 0;
    }

    z[0] = 0x78;                /* deflate, 32K window */
    z[1] = 0x01;                /* no preset dict, check bits make it %31 */
    pos = 2;
    off = 0;
    do {
        size_t n = rawLen - off;
        int final;

        if (n > 65535)
            n = 65535;
        final = (off + n >= rawLen);

        z[pos++] = (u8)(final ? 1 : 0);         /* BFINAL, BTYPE=00 stored */
        z[pos++] = (u8)(n & 0xFF);
        z[pos++] = (u8)(n >> 8);
        z[pos++] = (u8)(~n & 0xFF);
        z[pos++] = (u8)((~n >> 8) & 0xFF);
        memcpy(z + pos, raw + off, n);
        pos += n;
        off += n;
    } while (off < rawLen);
    Be32(z + pos, (s2 << 16) | s1);
    pos += 4;

    Be32(ihdr, (u32)w);
    Be32(ihdr + 4, (u32)h);
    ihdr[8] = 8;                /* bit depth */
    ihdr[9] = 2;                /* colour type: truecolour */
    ihdr[10] = 0;               /* deflate */
    ihdr[11] = 0;               /* adaptive filtering */
    ihdr[12] = 0;               /* no interlace */

    f = fopen(path, "wb");
    if (f == NULL) {
        free(raw);
        free(z);
        return 0;
    }
    ok = fwrite(kSig, 1, 8, f) == 8
      && Chunk(f, "IHDR", ihdr, sizeof(ihdr))
      && Chunk(f, "IDAT", z, pos)
      && Chunk(f, "IEND", NULL, 0);
    if (fclose(f) != 0)
        ok = 0;

    free(raw);
    free(z);
    return ok;
}
