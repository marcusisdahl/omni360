/*
FILE : Nzb.cpp
PROJECT : Omni360
DESCRIPTION : Reading NZB files. See Nzb.h.
*/

#include "Nzb.h"

#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

// XML's five entities and numeric references (ASCII ones - that's all an
// NZB's subjects and message IDs carry in practice), in place.
static void DecodeEntities(std::string &s)
{
    std::string out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length();)
    {
        if (s[i] == '&')
        {
            const size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 8)
            {
                const std::string name = s.substr(i + 1, semi - i - 1);
                unsigned long code = 0;
                if (name == "amp") code = '&';
                else if (name == "lt") code = '<';
                else if (name == "gt") code = '>';
                else if (name == "quot") code = '"';
                else if (name == "apos") code = '\'';
                else if (name.length() > 1 && name[0] == '#')
                    code = (name[1] == 'x' || name[1] == 'X') ? strtoul(name.c_str() + 2, NULL, 16)
                                                               : strtoul(name.c_str() + 1, NULL, 10);
                if (code > 0 && code < 0x80)
                {
                    out += (char)code;
                    i = semi + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    s.swap(out);
}

// attr="value" inside the tag [tag, tagEnd).
static bool Attribute(const char *tag, const char *tagEnd, const char *attr, std::string *out)
{
    const std::string key = std::string(attr) + "=\"";
    for (const char *p = tag + 1; p + key.length() < tagEnd; ++p)
    {
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r') &&
            memcmp(p, key.c_str(), key.length()) == 0)
        {
            const char *v = p + key.length();
            const char *q = (const char *)memchr(v, '"', tagEnd - v);
            if (q == NULL)
                return false;
            out->assign(v, q - v);
            DecodeEntities(*out);
            return true;
        }
    }
    return false;
}

bool NzbNameFromSubject(const char *subject, char *out, unsigned long outSize)
{
    const char *open = strchr(subject, '"');
    const char *close = (open != NULL) ? strchr(open + 1, '"') : NULL;
    if (close == NULL || close == open + 1 || outSize == 0)
        return false;
    unsigned long n = (unsigned long)(close - open - 1);
    if (n >= outSize)
        n = outSize - 1;
    memcpy(out, open + 1, n);
    out[n] = '\0';
    return true;
}

static bool EndsWithNoCase(const char *s, const char *suffix)
{
    const size_t n = strlen(s), m = strlen(suffix);
    if (n < m)
        return false;
    for (size_t i = 0; i < m; ++i)
    {
        char a = s[n - m + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

static bool ByNumber(const NzbSegment &a, const NzbSegment &b) { return a.number < b.number; }

bool NzbParse(const char *xml, Nzb *out)
{
    memset(out, 0, sizeof(*out));

    std::vector<NzbFile> files;
    std::vector<NzbSegment> segments;
    std::string ids;

    const char *p = xml;
    for (;;)
    {
        const char *file = strstr(p, "<file");
        if (file == NULL)
            break;
        if (file[5] != ' ' && file[5] != '\t' && file[5] != '\r' && file[5] != '\n')
        {
            p = file + 5; // <files>, or anything else that only starts the same
            continue;
        }
        const char *fileTagEnd = strchr(file, '>');
        const char *fileEnd = strstr(file, "</file>");
        if (fileTagEnd == NULL || fileEnd == NULL || fileTagEnd > fileEnd)
            break;
        p = fileEnd + 7;

        NzbFile f;
        memset(&f, 0, sizeof(f));
        std::string subject;
        if (Attribute(file, fileTagEnd, "subject", &subject))
            NzbNameFromSubject(subject.c_str(), f.name, sizeof(f.name));
        f.par2 = EndsWithNoCase(f.name, ".par2");
        f.firstSegment = (unsigned long)segments.size();

        const char *s = fileTagEnd;
        for (;;)
        {
            const char *seg = strstr(s, "<segment");
            if (seg == NULL || seg >= fileEnd)
                break;
            if (seg[8] != ' ' && seg[8] != '\t' && seg[8] != '\r' && seg[8] != '\n')
            {
                s = seg + 8; // <segments>
                continue;
            }
            const char *segTagEnd = strchr(seg, '>');
            const char *segEnd = strstr(seg, "</segment>");
            if (segTagEnd == NULL || segEnd == NULL || segTagEnd > segEnd || segEnd > fileEnd)
                break;
            s = segEnd + 10;

            std::string number, bytes;
            std::string id(segTagEnd + 1, segEnd - segTagEnd - 1);
            DecodeEntities(id);
            // Trimmed, and without <> if the NZB wrote them.
            while (!id.empty() && (id[0] == ' ' || id[0] == '\t' || id[0] == '\r' || id[0] == '\n' || id[0] == '<'))
                id.erase(0, 1);
            while (!id.empty() && (id[id.length() - 1] == ' ' || id[id.length() - 1] == '\t' ||
                                   id[id.length() - 1] == '\r' || id[id.length() - 1] == '\n' ||
                                   id[id.length() - 1] == '>'))
                id.erase(id.length() - 1);
            if (id.empty() || !Attribute(seg, segTagEnd, "number", &number))
                continue;
            Attribute(seg, segTagEnd, "bytes", &bytes);

            NzbSegment segment;
            segment.number = strtoul(number.c_str(), NULL, 10);
            segment.bytes = strtoul(bytes.c_str(), NULL, 10);
            segment.idAt = (unsigned long)ids.length();
            ids += id;
            ids += '\0';
            segments.push_back(segment);
        }

        f.segmentCount = (unsigned long)segments.size() - f.firstSegment;
        if (f.segmentCount == 0)
            continue;

        // In order, once each - an NZB may list them any way round, or twice.
        std::vector<NzbSegment>::iterator first = segments.begin() + f.firstSegment;
        std::stable_sort(first, segments.end(), ByNumber);
        unsigned long kept = 0;
        for (unsigned long i = 0; i < f.segmentCount; ++i)
        {
            const NzbSegment &seg = segments[f.firstSegment + i];
            if (kept > 0 && segments[f.firstSegment + kept - 1].number == seg.number)
                continue;
            segments[f.firstSegment + kept++] = seg;
        }
        segments.resize(f.firstSegment + kept);
        f.segmentCount = kept;
        for (unsigned long i = 0; i < kept; ++i)
            f.bytes += segments[f.firstSegment + i].bytes;
        files.push_back(f);
    }

    if (files.empty())
        return false;

    out->files = (NzbFile *)malloc(files.size() * sizeof(NzbFile));
    out->segments = (NzbSegment *)malloc(segments.size() * sizeof(NzbSegment));
    out->ids = (char *)malloc(ids.length() + 1);
    if (out->files == NULL || out->segments == NULL || out->ids == NULL)
    {
        NzbFree(out);
        return false;
    }
    memcpy(out->files, &files[0], files.size() * sizeof(NzbFile));
    memcpy(out->segments, &segments[0], segments.size() * sizeof(NzbSegment));
    memcpy(out->ids, ids.data(), ids.length());
    out->ids[ids.length()] = '\0';
    out->fileCount = (int)files.size();
    out->segmentCount = (unsigned long)segments.size();
    return true;
}

void NzbFree(Nzb *nzb)
{
    if (nzb == NULL)
        return;
    free(nzb->files);
    free(nzb->segments);
    free(nzb->ids);
    memset(nzb, 0, sizeof(*nzb));
}
