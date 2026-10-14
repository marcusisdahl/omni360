/*
FILE : Md5Digest.cpp
PROJECT : Omni360
DESCRIPTION : MD5, RFC 1321. See Md5Digest.h.
*/

#include "Md5Digest.h"

#include <string.h>

static unsigned long Rotate(unsigned long x, int n) { return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFFUL; }

// Byte by byte, so it's the same on the 360's big-endian CPU as on a PC.
static unsigned long LoadLE(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void StoreLE(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static const unsigned long kSine[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

static const int kShift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                               5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                               4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                               6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void Transform(unsigned long state[4], const unsigned char block[64])
{
    unsigned long m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = LoadLE(block + i * 4);

    unsigned long a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; ++i)
    {
        unsigned long f;
        int g;
        if (i < 16)
        {
            f = (b & c) | (~b & d);
            g = i;
        }
        else if (i < 32)
        {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        }
        else if (i < 48)
        {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        }
        else
        {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        const unsigned long next = d;
        d = c;
        c = b;
        b = (b + Rotate((a + (f & 0xFFFFFFFFUL) + kSine[i] + m[g]) & 0xFFFFFFFFUL, kShift[i])) & 0xFFFFFFFFUL;
        a = next;
    }
    state[0] = (state[0] + a) & 0xFFFFFFFFUL;
    state[1] = (state[1] + b) & 0xFFFFFFFFUL;
    state[2] = (state[2] + c) & 0xFFFFFFFFUL;
    state[3] = (state[3] + d) & 0xFFFFFFFFUL;
}

void Md5Init(Md5 *md5)
{
    md5->state[0] = 0x67452301;
    md5->state[1] = 0xefcdab89;
    md5->state[2] = 0x98badcfe;
    md5->state[3] = 0x10325476;
    md5->length = 0;
    md5->used = 0;
}

void Md5Update(Md5 *md5, const void *data, unsigned long len)
{
    const unsigned char *p = (const unsigned char *)data;
    md5->length += len;
    if (md5->used > 0)
    {
        unsigned long take = 64 - md5->used;
        if (take > len)
            take = len;
        memcpy(md5->block + md5->used, p, take);
        md5->used += take;
        p += take;
        len -= take;
        if (md5->used < 64)
            return;
        Transform(md5->state, md5->block);
        md5->used = 0;
    }
    while (len >= 64)
    {
        Transform(md5->state, p);
        p += 64;
        len -= 64;
    }
    memcpy(md5->block, p, len);
    md5->used = len;
}

void Md5Final(Md5 *md5, unsigned char digest[16])
{
    const unsigned long long bits = md5->length * 8;
    static const unsigned char pad[64] = {0x80};
    unsigned long padLen = (md5->used < 56) ? 56 - md5->used : 120 - md5->used;
    Md5Update(md5, pad, padLen);
    unsigned char lengthBytes[8];
    for (int i = 0; i < 8; ++i)
        lengthBytes[i] = (unsigned char)(bits >> (8 * i));
    Md5Update(md5, lengthBytes, 8);
    for (int i = 0; i < 4; ++i)
        StoreLE(digest + i * 4, md5->state[i]);
}

void Md5Of(const void *data, unsigned long len, unsigned char digest[16])
{
    Md5 md5;
    Md5Init(&md5);
    Md5Update(&md5, data, len);
    Md5Final(&md5, digest);
}
