/*
FILE : Par2.cpp
PROJECT : Omni360
DESCRIPTION : Reading par2 files, finding damaged slices and repairing them
              with Reed-Solomon over GF(2^16). See Par2.h.
*/

#include "Par2.h"
#include "Md5Digest.h"
#include "Yenc.h" // YencCrc32 - par2's CRC32 is the same one

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#define PACKET_HEADER   64
#define SMALL_PACKET_MAX (64UL * 1024 * 1024) // anything but a recovery slice is far smaller
#define CHUNK           (1024UL * 1024)

static const unsigned char kMagic[8] = {'P', 'A', 'R', '2', 0, 'P', 'K', 'T'};
static const unsigned char kTypeMain[16] = {'P', 'A', 'R', ' ', '2', '.', '0', 0, 'M', 'a', 'i', 'n', 0, 0, 0, 0};
static const unsigned char kTypeFileDesc[16] = {'P', 'A', 'R', ' ', '2', '.', '0', 0,
                                                'F', 'i', 'l', 'e', 'D', 'e', 's', 'c'};
static const unsigned char kTypeIfsc[16] = {'P', 'A', 'R', ' ', '2', '.', '0', 0, 'I', 'F', 'S', 'C', 0, 0, 0, 0};
static const unsigned char kTypeRecovery[16] = {'P', 'A', 'R', ' ', '2', '.', '0', 0,
                                                'R', 'e', 'c', 'v', 'S', 'l', 'i', 'c'};

static unsigned long long LE64(const unsigned char *p)
{
    unsigned long long v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

static unsigned long LE32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static unsigned long long FileLength(FILE *f)
{
    if (_fseeki64(f, 0, SEEK_END) != 0)
        return 0;
    const __int64 n = _ftelli64(f);
    return n > 0 ? (unsigned long long)n : 0;
}

// ---------------------------------------------------------------------------
// GF(2^16), with par2's generator x^16 + x^12 + x^3 + x + 1
// ---------------------------------------------------------------------------

static unsigned short g_log[65536];
static unsigned short g_exp[65535 * 2];
static bool g_gfReady = false;

static void GfInit()
{
    if (g_gfReady)
        return;
    unsigned long b = 1;
    for (unsigned long l = 0; l < 65535; ++l)
    {
        g_log[b] = (unsigned short)l;
        g_exp[l] = (unsigned short)b;
        g_exp[l + 65535] = (unsigned short)b;
        b <<= 1;
        if (b & 0x10000)
            b ^= 0x1100B;
    }
    g_log[0] = 0; // never used: 0 is handled on its own
    g_gfReady = true;
}

static unsigned short GfMul(unsigned short a, unsigned short b)
{
    return (a == 0 || b == 0) ? 0 : g_exp[g_log[a] + g_log[b]];
}

static unsigned short GfDiv(unsigned short a, unsigned short b)
{
    return (a == 0) ? 0 : g_exp[g_log[a] + 65535 - g_log[b]];
}

// base^exponent, for a base given by its log.
static unsigned short GfPowLog(unsigned long logBase, unsigned long exponent)
{
    return g_exp[(unsigned long)(((unsigned long long)logBase * exponent) % 65535)];
}

// The log of input slice i's base: the i-th positive whole number that shares
// no factor with 65535 (3 x 5 x 17 x 257).
static void SliceBaseLogs(unsigned long count, std::vector<unsigned long> *out)
{
    out->resize(count);
    unsigned long n = 1;
    for (unsigned long i = 0; i < count; ++i)
    {
        while (n % 3 == 0 || n % 5 == 0 || n % 17 == 0 || n % 257 == 0)
            n++;
        (*out)[i] = n++;
    }
}

// out ^= c * data, word by word - little-endian 16-bit words, as par2 has
// them, handled a byte at a time so it's the same on the 360.
static void MulAdd(unsigned char *out, const unsigned char *data, unsigned long len, unsigned short c)
{
    if (c == 0)
        return;
    unsigned short lo[256], hi[256];
    for (unsigned int x = 0; x < 256; ++x)
    {
        lo[x] = GfMul(c, (unsigned short)x);
        hi[x] = GfMul(c, (unsigned short)(x << 8));
    }
    for (unsigned long i = 0; i + 1 < len; i += 2)
    {
        const unsigned short v = lo[data[i]] ^ hi[data[i + 1]];
        out[i] ^= (unsigned char)v;
        out[i + 1] ^= (unsigned char)(v >> 8);
    }
}

// ---------------------------------------------------------------------------
// Packets
// ---------------------------------------------------------------------------

// Where the next packet's magic is, at or after `from`; size if none.
static unsigned long long FindMagic(FILE *f, unsigned long long from, unsigned long long size)
{
    std::vector<unsigned char> buffer(CHUNK + 8);
    while (from + 8 <= size)
    {
        if (_fseeki64(f, (__int64)from, SEEK_SET) != 0)
            break;
        const size_t got = fread(&buffer[0], 1, buffer.size(), f);
        if (got < 8)
            break;
        for (size_t i = 0; i + 8 <= got; ++i)
        {
            if (buffer[i] == 'P' && memcmp(&buffer[i], kMagic, 8) == 0)
                return from + i;
        }
        from += got - 7;
    }
    return size;
}

// Calls visit(header, offset, f, size) for each packet in a file whose header
// looks sound; visit says whether the packet checked out, so a damaged one is
// stepped over and the next found by its magic.
struct PacketVisitor
{
    virtual ~PacketVisitor() {}
    virtual bool Visit(FILE *f, const unsigned char *header, unsigned long long offset, unsigned long long length) = 0;
};

static void ScanPackets(const std::string &path, PacketVisitor *visitor)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (f == NULL)
        return;
    const unsigned long long size = FileLength(f);
    unsigned long long at = 0;
    while (at + PACKET_HEADER <= size)
    {
        unsigned char header[PACKET_HEADER];
        bool ok = _fseeki64(f, (__int64)at, SEEK_SET) == 0 && fread(header, 1, PACKET_HEADER, f) == PACKET_HEADER &&
                  memcmp(header, kMagic, 8) == 0;
        const unsigned long long length = ok ? LE64(header + 8) : 0;
        ok = ok && length >= PACKET_HEADER && length % 4 == 0 && at + length <= size;
        if (ok && visitor->Visit(f, header, at, length))
            at += length;
        else
            at = FindMagic(f, at + 1, size);
    }
    fclose(f);
}

// A whole packet, read and checked against the MD5 in its header.
static bool ReadPacket(FILE *f, unsigned long long offset, unsigned long long length, std::vector<unsigned char> *out)
{
    if (length > SMALL_PACKET_MAX)
        return false;
    out->resize((size_t)length);
    if (_fseeki64(f, (__int64)offset, SEEK_SET) != 0 || fread(&(*out)[0], 1, (size_t)length, f) != length)
        return false;
    unsigned char digest[16];
    Md5Of(&(*out)[32], (unsigned long)(length - 32), digest);
    return memcmp(digest, &(*out)[16], 16) == 0;
}

struct IndexCollector : PacketVisitor
{
    bool haveMain;
    unsigned char setId[16];
    unsigned long long sliceSize;
    std::vector<std::string> fileIds; // the recovery set's, in order
    std::vector<std::vector<unsigned char> > descs, ifscs;

    IndexCollector() : haveMain(false), sliceSize(0) {}

    bool Visit(FILE *f, const unsigned char *header, unsigned long long offset, unsigned long long length)
    {
        const unsigned char *type = header + 48;
        const bool main = memcmp(type, kTypeMain, 16) == 0, desc = memcmp(type, kTypeFileDesc, 16) == 0,
                   ifsc = memcmp(type, kTypeIfsc, 16) == 0;
        if (!main && !desc && !ifsc)
            return true; // a recovery slice or a creator's note: not needed here, and stepped over unread
        if (haveMain && memcmp(header + 32, setId, 16) != 0)
            return true; // another set's

        std::vector<unsigned char> packet;
        if (!ReadPacket(f, offset, length, &packet))
            return false;
        const unsigned char *body = &packet[PACKET_HEADER];
        const unsigned long long bodyLen = length - PACKET_HEADER;
        if (main && !haveMain && bodyLen >= 12)
        {
            const unsigned long count = LE32(body + 8);
            if (12 + (unsigned long long)count * 16 > bodyLen)
                return false;
            haveMain = true;
            memcpy(setId, header + 32, 16);
            sliceSize = LE64(body);
            for (unsigned long i = 0; i < count; ++i)
                fileIds.push_back(std::string((const char *)body + 12 + i * 16, 16));
        }
        else if (desc && bodyLen >= 56)
            descs.push_back(packet);
        else if (ifsc && bodyLen >= 16)
            ifscs.push_back(packet);
        return true;
    }
};

bool Par2ReadSet(const std::vector<std::string> &par2Paths, Par2Set *out, std::string *error)
{
    IndexCollector index;
    for (size_t i = 0; i < par2Paths.size(); ++i)
        ScanPackets(par2Paths[i], &index);
    if (!index.haveMain || index.sliceSize == 0 || index.sliceSize % 4 != 0)
    {
        *error = "The par2 index has no usable main packet.";
        return false;
    }

    memcpy(out->setId, index.setId, 16);
    out->sliceSize = index.sliceSize;
    out->files.clear();
    out->sliceCount = 0;
    for (size_t i = 0; i < index.fileIds.size(); ++i)
    {
        Par2File file;
        memcpy(file.id, index.fileIds[i].data(), 16);
        bool haveDesc = false, haveIfsc = false;
        for (size_t d = 0; d < index.descs.size() && !haveDesc; ++d)
        {
            const std::vector<unsigned char> &p = index.descs[d];
            if (memcmp(&p[32], index.setId, 16) != 0 || memcmp(&p[PACKET_HEADER], file.id, 16) != 0)
                continue;
            const unsigned char *body = &p[PACKET_HEADER];
            memcpy(file.hash16k, body + 32, 16);
            file.length = LE64(body + 48);
            const char *name = (const char *)body + 56;
            size_t nameLen = p.size() - PACKET_HEADER - 56;
            while (nameLen > 0 && name[nameLen - 1] == '\0')
                nameLen--;
            file.name.assign(name, nameLen);
            haveDesc = true;
        }
        const unsigned long slices = (unsigned long)((file.length + index.sliceSize - 1) / index.sliceSize);
        for (size_t s = 0; s < index.ifscs.size() && !haveIfsc && haveDesc; ++s)
        {
            const std::vector<unsigned char> &p = index.ifscs[s];
            if (memcmp(&p[32], index.setId, 16) != 0 || memcmp(&p[PACKET_HEADER], file.id, 16) != 0)
                continue;
            if (p.size() < PACKET_HEADER + 16 + (size_t)slices * 20)
                continue;
            for (unsigned long k = 0; k < slices; ++k)
                file.crc.push_back(LE32(&p[PACKET_HEADER + 16 + k * 20 + 16]));
            haveIfsc = true;
        }
        if (!haveDesc || !haveIfsc)
        {
            *error = "The par2 index is missing a file's description or checksums.";
            return false;
        }
        file.firstSlice = out->sliceCount;
        file.sliceCount = slices;
        out->sliceCount += slices;
        out->files.push_back(file);
    }
    return true;
}

int Par2VolumeBlocks(const char *name)
{
    // "name.vol07+08.par2" - the count after the '+'.
    const size_t n = strlen(name);
    if (n < 5 || _stricmp(name + n - 5, ".par2") != 0)
        return 0;
    const char *vol = NULL;
    for (const char *p = name; (p = strstr(p, ".vol")) != NULL; ++p)
        vol = p;
    if (vol == NULL)
    {
        for (const char *p = name; (p = strstr(p, ".VOL")) != NULL; ++p)
            vol = p;
    }
    if (vol == NULL)
        return 0;
    const char *plus = strchr(vol, '+');
    return (plus != NULL && plus < name + n - 5) ? atoi(plus + 1) : 0;
}

// ---------------------------------------------------------------------------
// Matching, checking
// ---------------------------------------------------------------------------

static bool Hash16k(const std::string &path, unsigned long long *length, unsigned char digest[16])
{
    FILE *f = fopen(path.c_str(), "rb");
    if (f == NULL)
        return false;
    *length = FileLength(f);
    unsigned char buffer[16384];
    const size_t want = *length < sizeof(buffer) ? (size_t)*length : sizeof(buffer);
    const bool ok = _fseeki64(f, 0, SEEK_SET) == 0 && fread(buffer, 1, want, f) == want;
    fclose(f);
    if (ok)
        Md5Of(buffer, (unsigned long)want, digest);
    return ok;
}

void Par2MatchFiles(const Par2Set &set, const std::vector<std::string> &names, const std::vector<std::string> &paths,
                    std::vector<int> *fileForPar)
{
    fileForPar->assign(set.files.size(), -1);
    std::vector<bool> taken(paths.size(), false);

    // By name first.
    for (size_t i = 0; i < set.files.size(); ++i)
    {
        for (size_t j = 0; j < names.size(); ++j)
        {
            if (!taken[j] && _stricmp(names[j].c_str(), set.files[i].name.c_str()) == 0)
            {
                (*fileForPar)[i] = (int)j;
                taken[j] = true;
                break;
            }
        }
    }

    // Then what's left, by the MD5 of its first 16KB - a file posted under a
    // made-up name.
    for (size_t j = 0; j < paths.size(); ++j)
    {
        if (taken[j])
            continue;
        unsigned long long length = 0;
        unsigned char digest[16];
        if (!Hash16k(paths[j], &length, digest))
            continue;
        for (size_t i = 0; i < set.files.size(); ++i)
        {
            if ((*fileForPar)[i] < 0 && memcmp(digest, set.files[i].hash16k, 16) == 0)
            {
                (*fileForPar)[i] = (int)j;
                taken[j] = true;
                break;
            }
        }
    }
}

// Reads slice `index` of a file, zero-padded to a whole slice. False if the
// file couldn't be read at all; a short file reads as zeros past its end.
static bool ReadSlice(FILE *f, const Par2Set &set, const Par2File &file, unsigned long index, unsigned char *out)
{
    memset(out, 0, (size_t)set.sliceSize);
    if (f == NULL)
        return false;
    const unsigned long long offset = (unsigned long long)index * set.sliceSize;
    unsigned long long want = file.length - offset;
    if (want > set.sliceSize)
        want = set.sliceSize;
    if (_fseeki64(f, (__int64)offset, SEEK_SET) != 0)
        return true;
    fread(out, 1, (size_t)want, f);
    return true;
}

bool Par2FindDamaged(const Par2Set &set, const std::vector<std::string> &paths, const std::vector<bool> &check,
                     std::vector<unsigned long> *damaged, Par2ProgressFn progress, void *context)
{
    damaged->clear();
    unsigned long long total = 0, done = 0;
    for (size_t i = 0; i < set.files.size(); ++i)
    {
        if (i < check.size() && check[i] && i < paths.size() && !paths[i].empty())
            total += set.files[i].length;
    }

    std::vector<unsigned char> slice((size_t)set.sliceSize);
    for (size_t i = 0; i < set.files.size(); ++i)
    {
        const Par2File &file = set.files[i];
        const bool found = i < paths.size() && !paths[i].empty();
        if (!found)
        {
            for (unsigned long s = 0; s < file.sliceCount; ++s)
                damaged->push_back(file.firstSlice + s);
            continue;
        }
        if (i >= check.size() || !check[i])
            continue;

        FILE *f = fopen(paths[i].c_str(), "rb");
        for (unsigned long s = 0; s < file.sliceCount; ++s)
        {
            ReadSlice(f, set, file, s, &slice[0]);
            if (f == NULL || YencCrc32(0, &slice[0], (unsigned long)set.sliceSize) != file.crc[s])
                damaged->push_back(file.firstSlice + s);
            done += set.sliceSize;
            if (progress != NULL && !progress(done < total ? done : total, total, context))
            {
                if (f != NULL)
                    fclose(f);
                return false;
            }
        }
        if (f != NULL)
            fclose(f);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Recovery slices
// ---------------------------------------------------------------------------

struct RecoveryCollector : PacketVisitor
{
    const Par2Set *set;
    std::string path;
    std::vector<Par2Recovery> *out;

    bool Visit(FILE *f, const unsigned char *header, unsigned long long offset, unsigned long long length)
    {
        if (memcmp(header + 48, kTypeRecovery, 16) != 0 || memcmp(header + 32, set->setId, 16) != 0)
            return true;
        if (length != PACKET_HEADER + 4 + set->sliceSize)
            return false;

        // Checked against its MD5 a chunk at a time - a slice can be megabytes.
        Md5 md5;
        Md5Init(&md5);
        Md5Update(&md5, header + 32, 32);
        std::vector<unsigned char> buffer(CHUNK);
        unsigned long long at = offset + PACKET_HEADER, left = length - PACKET_HEADER;
        unsigned char exponentBytes[4] = {0, 0, 0, 0};
        bool first = true;
        if (_fseeki64(f, (__int64)at, SEEK_SET) != 0)
            return false;
        while (left > 0)
        {
            const size_t take = left < buffer.size() ? (size_t)left : buffer.size();
            if (fread(&buffer[0], 1, take, f) != take)
                return false;
            if (first)
            {
                memcpy(exponentBytes, &buffer[0], 4);
                first = false;
            }
            Md5Update(&md5, &buffer[0], (unsigned long)take);
            left -= take;
        }
        unsigned char digest[16];
        Md5Final(&md5, digest);
        if (memcmp(digest, header + 16, 16) != 0)
            return false;

        Par2Recovery r;
        r.path = path;
        r.offset = offset + PACKET_HEADER + 4;
        r.exponent = LE32(exponentBytes);
        for (size_t i = 0; i < out->size(); ++i)
        {
            if ((*out)[i].exponent == r.exponent)
                return true;
        }
        out->push_back(r);
        return true;
    }
};

void Par2ListRecovery(const Par2Set &set, const std::vector<std::string> &volumePaths, std::vector<Par2Recovery> *out)
{
    RecoveryCollector collector;
    collector.set = &set;
    collector.out = out;
    for (size_t i = 0; i < volumePaths.size(); ++i)
    {
        collector.path = volumePaths[i];
        ScanPackets(volumePaths[i], &collector);
    }
}

// ---------------------------------------------------------------------------
// Repair
// ---------------------------------------------------------------------------

// The inverse of a k x k matrix over GF(2^16), in place, by Gauss-Jordan.
// False if it has none.
static bool Invert(std::vector<unsigned short> &m, size_t k)
{
    std::vector<unsigned short> inv(k * k, 0);
    for (size_t i = 0; i < k; ++i)
        inv[i * k + i] = 1;
    for (size_t col = 0; col < k; ++col)
    {
        size_t pivot = col;
        while (pivot < k && m[pivot * k + col] == 0)
            pivot++;
        if (pivot == k)
            return false;
        if (pivot != col)
        {
            for (size_t c = 0; c < k; ++c)
            {
                std::swap(m[pivot * k + c], m[col * k + c]);
                std::swap(inv[pivot * k + c], inv[col * k + c]);
            }
        }
        const unsigned short p = m[col * k + col];
        for (size_t c = 0; c < k; ++c)
        {
            m[col * k + c] = GfDiv(m[col * k + c], p);
            inv[col * k + c] = GfDiv(inv[col * k + c], p);
        }
        for (size_t r = 0; r < k; ++r)
        {
            const unsigned short factor = m[r * k + col];
            if (r == col || factor == 0)
                continue;
            for (size_t c = 0; c < k; ++c)
            {
                m[r * k + c] ^= GfMul(factor, m[col * k + c]);
                inv[r * k + c] ^= GfMul(factor, inv[col * k + c]);
            }
        }
    }
    m.swap(inv);
    return true;
}

// The file and slice-within-it of a slice number across the set.
static size_t FileOfSlice(const Par2Set &set, unsigned long slice)
{
    size_t f = 0;
    while (f + 1 < set.files.size() && set.files[f + 1].firstSlice <= slice)
        f++;
    return f;
}

Par2Result Par2Repair(const Par2Set &set, const std::vector<std::string> &paths,
                      const std::vector<unsigned long> &damaged, const std::vector<Par2Recovery> &recovery,
                      unsigned long long memoryBudget, Par2ProgressFn progress, void *context, std::string *error)
{
    GfInit();
    const size_t k = damaged.size();
    if (k == 0)
        return PAR2_REPAIRED;
    if (recovery.size() < k)
    {
        char text[128];
        _snprintf(text, sizeof(text), "%u damaged slices, and %u recovery slices to repair them with.", (unsigned)k,
                  (unsigned)recovery.size());
        text[sizeof(text) - 1] = '\0';
        *error = text;
        return PAR2_NOT_ENOUGH;
    }

    std::vector<unsigned long> logs;
    SliceBaseLogs(set.sliceCount, &logs);

    // The equations: each recovery slice r is the sum of base_i^e_r * slice_i
    // over every slice i. Moving the intact slices' part to the other side
    // leaves k equations in the k damaged ones - solvable by inverting their
    // coefficients. Most k-sets of recovery slices are; if the first k
    // aren't, the next ones are tried.
    std::vector<unsigned short> inverse;
    std::vector<size_t> used;
    for (size_t start = 0; start + k <= recovery.size(); ++start)
    {
        used.clear();
        for (size_t r = 0; r < k; ++r)
            used.push_back(start + r);
        inverse.assign(k * k, 0);
        for (size_t r = 0; r < k; ++r)
        {
            for (size_t c = 0; c < k; ++c)
                inverse[r * k + c] = GfPowLog(logs[damaged[c]], recovery[used[r]].exponent);
        }
        if (Invert(inverse, k))
            break;
        used.clear();
    }
    if (used.empty())
    {
        *error = "The recovery slices can't be solved for these damaged ones.";
        return PAR2_FAILED;
    }

    std::vector<bool> isDamaged(set.sliceCount, false);
    for (size_t i = 0; i < k; ++i)
        isDamaged[damaged[i]] = true;

    // As many damaged slices at a time as fit the budget; the intact ones are
    // read once for each batch.
    const size_t sliceSize = (size_t)set.sliceSize;
    size_t perBatch = (size_t)(memoryBudget / set.sliceSize);
    if (perBatch < 1)
        perBatch = 1;
    if (perBatch > k)
        perBatch = k;
    const size_t batches = (k + perBatch - 1) / perBatch;
    const unsigned long long total =
        (unsigned long long)batches * ((set.sliceCount - k) + k) * set.sliceSize;
    unsigned long long done = 0;

    std::vector<unsigned char> input(sliceSize);
    std::vector<unsigned short> coefficient(perBatch);
    Par2Result result = PAR2_REPAIRED;

    for (size_t first = 0; first < k && result == PAR2_REPAIRED; first += perBatch)
    {
        const size_t count = std::min(perBatch, k - first);
        std::vector<unsigned char> out(count * sliceSize, 0);

        // Every intact slice, into each damaged one it helps rebuild.
        FILE *f = NULL;
        size_t open = (size_t)-1;
        for (unsigned long slice = 0; slice < set.sliceCount && result == PAR2_REPAIRED; ++slice)
        {
            if (isDamaged[slice])
                continue;
            const size_t fileIndex = FileOfSlice(set, slice);
            if (fileIndex != open)
            {
                if (f != NULL)
                    fclose(f);
                f = fopen(paths[fileIndex].c_str(), "rb");
                open = fileIndex;
            }
            const Par2File &file = set.files[fileIndex];
            if (!ReadSlice(f, set, file, slice - file.firstSlice, &input[0]))
            {
                *error = "A file couldn't be read: " + file.name;
                result = PAR2_FAILED;
                break;
            }
            for (size_t j = 0; j < count; ++j)
            {
                // sum over r of inverse[j][r] * base_slice^e_r
                unsigned short c = 0;
                for (size_t r = 0; r < k; ++r)
                    c ^= GfMul(inverse[(first + j) * k + r], GfPowLog(logs[slice], recovery[used[r]].exponent));
                MulAdd(&out[j * sliceSize], &input[0], (unsigned long)sliceSize, c);
            }
            done += set.sliceSize;
            if (progress != NULL && !progress(done, total, context))
                result = PAR2_CANCELLED;
        }
        if (f != NULL)
            fclose(f);

        // And the recovery slices themselves.
        for (size_t r = 0; r < k && result == PAR2_REPAIRED; ++r)
        {
            const Par2Recovery &rec = recovery[used[r]];
            FILE *rf = fopen(rec.path.c_str(), "rb");
            const bool ok = rf != NULL && _fseeki64(rf, (__int64)rec.offset, SEEK_SET) == 0 &&
                            fread(&input[0], 1, sliceSize, rf) == sliceSize;
            if (rf != NULL)
                fclose(rf);
            if (!ok)
            {
                *error = "A recovery slice couldn't be read.";
                result = PAR2_FAILED;
                break;
            }
            for (size_t j = 0; j < count; ++j)
                MulAdd(&out[j * sliceSize], &input[0], (unsigned long)sliceSize, inverse[(first + j) * k + r]);
            done += set.sliceSize;
            if (progress != NULL && !progress(done, total, context))
                result = PAR2_CANCELLED;
        }

        // Checked against the index, then written where they belong.
        for (size_t j = 0; j < count && result == PAR2_REPAIRED; ++j)
        {
            const unsigned long slice = damaged[first + j];
            const size_t fileIndex = FileOfSlice(set, slice);
            const Par2File &file = set.files[fileIndex];
            const unsigned long within = slice - file.firstSlice;
            if (YencCrc32(0, &out[j * sliceSize], (unsigned long)sliceSize) != file.crc[within])
            {
                *error = "A repaired slice didn't match its checksum: " + file.name;
                result = PAR2_FAILED;
                break;
            }
            FILE *w = fopen(paths[fileIndex].c_str(), "r+b");
            if (w == NULL)
                w = fopen(paths[fileIndex].c_str(), "w+b");
            const unsigned long long offset = (unsigned long long)within * set.sliceSize;
            unsigned long long len = file.length - offset;
            if (len > set.sliceSize)
                len = set.sliceSize;
            const bool ok = w != NULL && _fseeki64(w, (__int64)offset, SEEK_SET) == 0 &&
                            fwrite(&out[j * sliceSize], 1, (size_t)len, w) == len;
            if (w != NULL && fclose(w) != 0)
                result = PAR2_FAILED;
            if (!ok)
            {
                *error = "A repaired slice couldn't be written: " + file.name;
                result = PAR2_FAILED;
            }
        }
    }
    return result;
}
