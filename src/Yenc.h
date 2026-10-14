#ifndef YENC_H
#define YENC_H

// ---------------------------------------------------------------------------
// yEnc - how binaries are posted to Usenet
// ---------------------------------------------------------------------------
//
// An article's body is one part of a file:
//
//   =ybegin part=3 total=40 line=128 size=104857600 name=game.part01.rar
//   =ypart begin=1536001 end=2304000
//   <data lines: each byte + 42, with NUL, LF, CR and '=' escaped as '='
//    followed by the byte + 64>
//   =yend size=768000 part=3 pcrc32=1a2b3c4d
//
// A single-part post has no =ypart line, and is the whole file.
//
// The body handed in has had NNTP's dot-stuffing undone already ("..x" at a
// line's start back to ".x"); line endings may be CRLF or LF.
//
// Free of any Xbox header, so it's tested on a PC (tools/test_usenet).

#define YENC_NAME_MAX 256

enum YencResult
{
    YENC_OK,
    YENC_NOT_YENC,      // no =ybegin line
    YENC_TRUNCATED,     // no =yend, or fewer bytes than =ypart says
    YENC_CRC_MISMATCH,  // decoded, but the bytes aren't what pcrc32 says
    YENC_TOO_BIG        // the part doesn't fit the output buffer
};

struct YencPart
{
    unsigned long long fileSize; // =ybegin size: the whole file's
    unsigned long long offset;   // where this part goes in the file (=ypart begin - 1)
    unsigned long length;        // bytes decoded
    char name[YENC_NAME_MAX];    // =ybegin name
};

// Decodes one article body into out (outCap bytes). On YENC_OK, *part says
// what it is and where it goes. On YENC_CRC_MISMATCH *part is filled too, and
// out holds the damaged bytes.
YencResult YencDecode(const char *body, unsigned long len, unsigned char *out, unsigned long outCap,
                      YencPart *part);

// CRC-32 (the zip/yEnc one), continued from crc - pass 0 to start.
unsigned long YencCrc32(unsigned long crc, const unsigned char *data, unsigned long len);

#endif
