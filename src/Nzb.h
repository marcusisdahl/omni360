#ifndef NZB_H
#define NZB_H

// ---------------------------------------------------------------------------
// NZB files - what a Usenet post is made of
// ---------------------------------------------------------------------------
//
//   <nzb>
//     <file poster="..." date="..." subject="Game [01/52] - &quot;game.part01.rar&quot; yEnc (1/137)">
//       <groups><group>alt.binaries.example</group></groups>
//       <segments>
//         <segment bytes="768000" number="1">part1of137.abc@example</segment>
//         ...
//
// Each <file> is one file of the post, each <segment> one article holding a
// yEnc part of it (Yenc.h), fetched by its message ID. The file's name is
// usually the quoted part of its subject; when a subject has none, the
// first article's =ybegin name says it instead.
//
// Free of any Xbox header, so it's tested on a PC (tools/test_usenet).

#define NZB_NAME_MAX 256

struct NzbSegment
{
    unsigned long number;  // 1-based, the part's place in its file
    unsigned long bytes;   // the article's size, yEnc and all - a little more than it decodes to
    unsigned long idAt;    // its message ID, at Nzb::ids + idAt, without the <>
};

struct NzbFile
{
    char name[NZB_NAME_MAX]; // empty when the subject didn't say
    unsigned long firstSegment; // its segments, in order, at Nzb::segments + firstSegment
    unsigned long segmentCount;
    unsigned long long bytes;   // the segments' bytes, added up
    bool par2;                  // a .par2 file - repair data
    bool want;                  // to download - everything but .par2 files, to start with
    unsigned long missing;      // articles that couldn't be had, once downloaded
};

struct Nzb
{
    NzbFile *files;
    int fileCount;
    NzbSegment *segments;
    unsigned long segmentCount;
    char *ids; // the message IDs, NUL-separated
};

// Parses an NZB. False if it holds no files with segments. On success *out
// owns its memory, for NzbFree.
bool NzbParse(const char *xml, Nzb *out);
void NzbFree(Nzb *nzb);

inline const char *NzbSegmentId(const Nzb &nzb, const NzbSegment &segment) { return nzb.ids + segment.idAt; }

// The quoted file name in a post's subject: Name [1/9] - "file.rar" yEnc (1/50).
// False if there's none.
bool NzbNameFromSubject(const char *subject, char *out, unsigned long outSize);

#endif
