/*
FILE : Yenc.cpp
PROJECT : Omni360
DESCRIPTION : Decoding yEnc article bodies. See Yenc.h.
*/

#include "Yenc.h"

#include <stdlib.h>
#include <string.h>

// Slicing-by-4: four tables, four bytes a step - several times the speed of
// one byte at a time, which matters at the 360's clock. The bytes are put
// together by hand, so it's the same on its big-endian CPU.
static unsigned long g_crcTable[4][256];
static bool g_crcReady = false;

unsigned long YencCrc32(unsigned long crc, const unsigned char *data, unsigned long len)
{
    if (!g_crcReady)
    {
        for (unsigned long i = 0; i < 256; ++i)
        {
            unsigned long c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            g_crcTable[0][i] = c;
        }
        for (unsigned long i = 0; i < 256; ++i)
        {
            for (int t = 1; t < 4; ++t)
                g_crcTable[t][i] = (g_crcTable[t - 1][i] >> 8) ^ g_crcTable[0][g_crcTable[t - 1][i] & 0xFF];
        }
        g_crcReady = true;
    }
    crc = ~crc & 0xFFFFFFFFUL;
    unsigned long i = 0;
    for (; i + 4 <= len; i += 4)
    {
        crc ^= (unsigned long)data[i] | ((unsigned long)data[i + 1] << 8) | ((unsigned long)data[i + 2] << 16) |
               ((unsigned long)data[i + 3] << 24);
        crc = g_crcTable[3][crc & 0xFF] ^ g_crcTable[2][(crc >> 8) & 0xFF] ^ g_crcTable[1][(crc >> 16) & 0xFF] ^
              g_crcTable[0][(crc >> 24) & 0xFF];
    }
    for (; i < len; ++i)
        crc = g_crcTable[0][(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc & 0xFFFFFFFFUL;
}

// The value of "key=" on a =y line (one that ends at lineEnd), as a number.
static bool KeyNumber(const char *line, const char *lineEnd, const char *key, unsigned long long *out, int base)
{
    const size_t keyLen = strlen(key);
    for (const char *p = line; p + keyLen < lineEnd; ++p)
    {
        // At a word's start, so "size=" doesn't match inside "psize=".
        if ((p == line || p[-1] == ' ') && memcmp(p, key, keyLen) == 0)
        {
            char digits[24];
            size_t n = 0;
            for (const char *d = p + keyLen; d < lineEnd && n < sizeof(digits) - 1 && *d != ' ' && *d != '\r'; ++d)
                digits[n++] = *d;
            digits[n] = '\0';
            if (n == 0)
                return false;
            *out = _strtoui64(digits, NULL, base);
            return true;
        }
    }
    return false;
}

static bool StartsWith(const char *line, const char *lineEnd, const char *prefix)
{
    const size_t n = strlen(prefix);
    return (size_t)(lineEnd - line) >= n && memcmp(line, prefix, n) == 0;
}

YencResult YencDecode(const char *body, unsigned long len, unsigned char *out, unsigned long outCap,
                      YencPart *part)
{
    memset(part, 0, sizeof(*part));
    const char *p = body, *end = body + len;

    bool begun = false, ended = false, haveCrc = false;
    unsigned long long partBegin = 0, partEnd = 0, crcValue = 0;
    unsigned long n = 0;

    while (p < end && !ended)
    {
        const char *lineEnd = (const char *)memchr(p, '\n', end - p);
        const char *next = (lineEnd != NULL) ? lineEnd + 1 : end;
        if (lineEnd == NULL)
            lineEnd = end;
        const char *contentEnd = lineEnd;
        if (contentEnd > p && contentEnd[-1] == '\r')
            contentEnd--;

        if (!begun)
        {
            if (StartsWith(p, contentEnd, "=ybegin "))
            {
                begun = true;
                KeyNumber(p, contentEnd, "size=", &part->fileSize, 10);
                // The name is the rest of the line, spaces and all.
                const char *name = NULL;
                for (const char *q = p + 1; q + 5 <= contentEnd; ++q)
                {
                    if (q[-1] == ' ' && memcmp(q, "name=", 5) == 0)
                    {
                        name = q + 5;
                        break;
                    }
                }
                if (name != NULL)
                {
                    size_t nameLen = contentEnd - name;
                    if (nameLen >= sizeof(part->name))
                        nameLen = sizeof(part->name) - 1;
                    memcpy(part->name, name, nameLen);
                    part->name[nameLen] = '\0';
                }
                partBegin = 1;
                partEnd = part->fileSize;
            }
        }
        else if (StartsWith(p, contentEnd, "=ypart "))
        {
            KeyNumber(p, contentEnd, "begin=", &partBegin, 10);
            KeyNumber(p, contentEnd, "end=", &partEnd, 10);
        }
        else if (StartsWith(p, contentEnd, "=yend"))
        {
            ended = true;
            haveCrc = KeyNumber(p, contentEnd, "pcrc32=", &crcValue, 16);
            // A single-part post may give only crc32=, the whole file's - the
            // same thing then.
            if (!haveCrc && partBegin == 1 && partEnd == part->fileSize)
                haveCrc = KeyNumber(p, contentEnd, "crc32=", &crcValue, 16);
        }
        else
        {
            for (const char *q = p; q < contentEnd; ++q)
            {
                unsigned char c = (unsigned char)*q;
                if (c == '=')
                {
                    if (++q >= contentEnd)
                        break; // an escape can't straddle lines; a stray '=' at the end is dropped
                    c = (unsigned char)(*q - 64);
                }
                if (n >= outCap)
                    return YENC_TOO_BIG;
                out[n++] = (unsigned char)(c - 42);
            }
        }
        p = next;
    }

    if (!begun)
        return YENC_NOT_YENC;

    part->offset = partBegin > 0 ? partBegin - 1 : 0;
    part->length = n;
    if (!ended || (partEnd >= partBegin && (unsigned long long)n < partEnd - partBegin + 1))
        return YENC_TRUNCATED;
    if (haveCrc && YencCrc32(0, out, n) != (unsigned long)crcValue)
        return YENC_CRC_MISMATCH;
    return YENC_OK;
}
