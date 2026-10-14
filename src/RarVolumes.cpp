/*
FILE : RarVolumes.cpp
PROJECT : Omni360
DESCRIPTION : A disc image read straight out of stored RAR volumes. See
              RarVolumes.h.
*/

#include "RarVolumes.h"

#include <stdlib.h>
#include <string.h>

static bool IsDigits(const char *s, size_t n)
{
    if (n == 0)
        return false;
    for (size_t i = 0; i < n; ++i)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

static char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool RarVolumeName(const char *name, std::string *base, int *order)
{
    const size_t n = strlen(name);
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name)
        return false;
    const char *ext = dot + 1;
    const size_t extLen = n - (ext - name);

    if (extLen == 3 && Lower(ext[0]) == 'r' && Lower(ext[1]) == 'a' && Lower(ext[2]) == 'r')
    {
        // "game.part01.rar", or the first of an old-style set, "game.rar".
        const std::string stem(name, dot - name);
        const size_t part = stem.rfind('.');
        if (part != std::string::npos && stem.length() - part > 5 &&
            _strnicmp(stem.c_str() + part + 1, "part", 4) == 0 &&
            IsDigits(stem.c_str() + part + 5, stem.length() - part - 5))
        {
            *base = stem.substr(0, part);
            *order = atoi(stem.c_str() + part + 5);
            return true;
        }
        *base = stem;
        *order = -1;
        return true;
    }

    // "game.r00" ... "game.r99", then "game.s00" ...
    if (extLen == 3 && (Lower(ext[0]) == 'r' || Lower(ext[0]) == 's') && IsDigits(ext + 1, 2))
    {
        *base = std::string(name, dot - name);
        *order = atoi(ext + 1) + (Lower(ext[0]) == 's' ? 100 : 0);
        return true;
    }
    return false;
}

static unsigned long long FileSizeOf(FILE *f)
{
    if (_fseeki64(f, 0, SEEK_END) != 0)
        return 0;
    const __int64 size = _ftelli64(f);
    return size > 0 ? (unsigned long long)size : 0;
}

// The headers of one volume, into out.
static RarWalkResult WalkVolume(const char *path, int fileIndex, std::vector<RarImage> *out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return RAR_WALK_UNREADABLE;
    const unsigned long long volumeSize = FileSizeOf(f);

    const unsigned long CHUNK = 65536;
    unsigned char *buffer = (unsigned char *)malloc(CHUNK);
    if (buffer == NULL)
    {
        fclose(f);
        return RAR_WALK_UNREADABLE;
    }

    RarWalkResult result = RAR_WALK_UNREADABLE;
    unsigned long signatureLen = 0;
    RarFormat format = RAR_FORMAT_UNKNOWN;
    if (_fseeki64(f, 0, SEEK_SET) == 0)
    {
        const size_t got = fread(buffer, 1, 16, f);
        format = RarDetectFormat(buffer, (unsigned long)got, &signatureLen);
    }

    unsigned long long at = signatureLen;
    // A volume holds a handful of headers; this is only a guard.
    for (int guard = 0; format != RAR_FORMAT_UNKNOWN && guard < 4096 && at < volumeSize; ++guard)
    {
        if (_fseeki64(f, (__int64)at, SEEK_SET) != 0)
            break;
        const unsigned long got = (unsigned long)fread(buffer, 1, CHUNK, f);
        RarEntry entry;
        unsigned long long next = 0;
        const int r = RarParseHeader(format, buffer, got, &entry, &next);
        if (r == RAR_HEADER_END)
        {
            result = RAR_WALK_OK;
            break;
        }
        if (r == RAR_HEADER_ENCRYPTED)
        {
            result = RAR_WALK_ENCRYPTED;
            break;
        }
        if (r != RAR_HEADER_OK || next == 0)
            break;

        if (entry.isFile)
        {
            // The same name in the next volume carries on the same file.
            RarImage *image = NULL;
            for (size_t i = 0; i < out->size(); ++i)
            {
                if ((*out)[i].name == entry.name)
                    image = &(*out)[i];
            }
            if (image == NULL)
            {
                out->push_back(RarImage());
                image = &out->back();
                image->name = entry.name;
                image->size = 0;
                image->stored = true;
                image->encrypted = false;
            }
            if (entry.unpSize > image->size)
                image->size = entry.unpSize;
            image->stored = image->stored && entry.stored;
            image->encrypted = image->encrypted || entry.encrypted;

            ImageSpan span;
            span.file = fileIndex;
            span.offset = at + entry.dataOffset;
            span.length = entry.packSize;
            if (span.offset + span.length <= volumeSize)
                image->spans.push_back(span);
        }
        at += next;
        if (at >= volumeSize)
            result = RAR_WALK_OK; // a volume may end without an end marker
    }

    free(buffer);
    fclose(f);
    return result;
}

RarWalkResult RarWalkVolumes(const std::vector<std::string> &paths, std::vector<RarImage> *out)
{
    out->clear();
    for (size_t i = 0; i < paths.size(); ++i)
    {
        const RarWalkResult r = WalkVolume(paths[i].c_str(), (int)i, out);
        if (r != RAR_WALK_OK)
            return r;
    }

    // A stored file's slices are the file - all of it, if every volume is here.
    for (size_t i = 0; i < out->size(); ++i)
    {
        const RarImage &image = (*out)[i];
        if (!image.stored || image.encrypted)
            continue;
        unsigned long long total = 0;
        for (size_t s = 0; s < image.spans.size(); ++s)
            total += image.spans[s].length;
        if (total != image.size)
            return RAR_WALK_INCOMPLETE;
    }
    return RAR_WALK_OK;
}

// ---------------------------------------------------------------------------
// SpanSource
// ---------------------------------------------------------------------------

SpanSource::SpanSource(const std::vector<std::string> &paths_, const std::vector<ImageSpan> &spans_)
    : paths(paths_), spans(spans_), size(0), file(NULL), openFile(-1)
{
    for (size_t i = 0; i < spans.size(); ++i)
    {
        starts.push_back(size);
        size += spans[i].length;
    }
}

SpanSource::~SpanSource()
{
    if (file != NULL)
        fclose(file);
}

bool SpanSource::ReadAt(unsigned long long offset, void *buffer, unsigned long len)
{
    unsigned char *outp = (unsigned char *)buffer;
    if (offset + len > size)
        return false;

    // The span holding offset: the last that starts at or before it.
    size_t lo = 0, hi = spans.size();
    while (hi - lo > 1)
    {
        const size_t mid = (lo + hi) / 2;
        if (starts[mid] <= offset)
            lo = mid;
        else
            hi = mid;
    }

    for (size_t s = lo; len > 0 && s < spans.size(); ++s)
    {
        const ImageSpan &span = spans[s];
        const unsigned long long within = offset - starts[s];
        if (within >= span.length)
            continue;

        if (span.file != openFile)
        {
            if (file != NULL)
                fclose(file);
            file = fopen(paths[span.file].c_str(), "rb");
            openFile = (file != NULL) ? span.file : -1;
            if (file == NULL)
                return false;
        }

        unsigned long take = (span.length - within < len) ? (unsigned long)(span.length - within) : len;
        if (_fseeki64(file, (__int64)(span.offset + within), SEEK_SET) != 0 || fread(outp, 1, take, file) != take)
            return false;
        outp += take;
        offset += take;
        len -= take;
    }
    return len == 0;
}
