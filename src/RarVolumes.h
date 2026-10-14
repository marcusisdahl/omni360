#ifndef RAR_VOLUMES_H
#define RAR_VOLUMES_H

#include "GodConvert.h" // GodSource
#include "RarHeaders.h" // RAR_NAME_MAX

#include <stdio.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Disc images out of downloaded files - for a Usenet download
// ---------------------------------------------------------------------------
//
// A post is usually a disc image cut into RAR volumes - game.part01.rar to
// game.part73.rar, or the older game.rar, game.r00, game.r01 ... Packed with
// "-m0" (stored, no compression), as console images normally are, the image
// is in the volumes byte for byte, a slice of it after each volume's file
// header. So instead of unpacking it, the volumes are read in order as one
// image: no RAR code beyond the headers, and no second copy of 8GB on the
// drive.
//
// A post of a bare .iso is the same thing with one slice.
//
// Free of any Xbox header, so it's tested on a PC (tools/test_usenet).

// Where a file's name says a RAR volume is in its set: *base is the set's
// name, *order its place in it. False if it isn't a RAR volume by name.
//   game.part01.rar -> "game", 1      game.rar -> "game", -1
//   game.r00        -> "game", 0      game.s00 -> "game", 100
bool RarVolumeName(const char *name, std::string *base, int *order);

// One slice of an image: bytes [offset, offset + length) of file `file`.
struct ImageSpan
{
    int file;
    unsigned long long offset;
    unsigned long long length;
};

struct RarImage
{
    std::string name;            // inside the archive
    unsigned long long size;     // once extracted
    std::vector<ImageSpan> spans;
    bool stored;                 // every slice is uncompressed
    bool encrypted;
};

enum RarWalkResult
{
    RAR_WALK_OK,
    RAR_WALK_UNREADABLE,  // a volume couldn't be opened, or isn't a RAR
    RAR_WALK_ENCRYPTED,   // its headers are
    RAR_WALK_INCOMPLETE   // a file's slices don't add up to its size - a volume is missing
};

// Reads the headers of each volume, paths[0] first, and lists the files
// they hold with their slices - file indexes in the slices index paths.
RarWalkResult RarWalkVolumes(const std::vector<std::string> &paths, std::vector<RarImage> *out);

// An image read from its slices across files.
class SpanSource : public GodSource
{
public:
    SpanSource(const std::vector<std::string> &paths, const std::vector<ImageSpan> &spans);
    ~SpanSource();

    bool ReadAt(unsigned long long offset, void *buffer, unsigned long len);
    unsigned long long Size() { return size; }

private:
    std::vector<std::string> paths;
    std::vector<ImageSpan> spans;
    std::vector<unsigned long long> starts; // each span's offset in the image
    unsigned long long size;
    FILE *file;
    int openFile;
};

#endif
