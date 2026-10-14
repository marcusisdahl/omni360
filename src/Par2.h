#ifndef PAR2_H
#define PAR2_H

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// par2 - repairing a Usenet download that's missing articles
// ---------------------------------------------------------------------------
//
// A post comes with .par2 files: a small index - for every file, its name,
// length, MD5 and the MD5 of its first 16KB, and a CRC32 and MD5 for each of
// its slices (pieces of one size, the post's "block size") - and volumes
// (name.vol07+08.par2) of recovery slices: Reed-Solomon sums over GF(2^16)
// of every slice of every file. With as many recovery slices as there are
// damaged ones, the damaged ones can be worked out again.
//
// So, NZBGet's way: the index is always downloaded; when articles were
// missing, the files they were in are checked slice by slice against it;
// only enough volumes are downloaded to cover the damaged slices; and they
// are repaired in place. The index also names files posted under random
// names - par2 knows each by the MD5 of its first 16KB.
//
// The format is the Parchive 2.0 specification's; tools/test_usenet checks
// this against par2cmdline's own files. Free of any Xbox header.

struct Par2File
{
    unsigned char id[16];
    unsigned char hash16k[16];        // MD5 of the first 16KB (or all of a smaller file)
    unsigned long long length;
    std::string name;
    std::vector<unsigned long> crc;   // each slice's CRC32, the last zero-padded to a whole slice
    unsigned long firstSlice;         // its first slice's number in the whole set
    unsigned long sliceCount;
};

struct Par2Set
{
    unsigned char setId[16];          // the recovery set's - every packet of it carries this
    unsigned long long sliceSize;
    std::vector<Par2File> files;      // the recovery set, in the order the slices are numbered
    unsigned long sliceCount;         // across every file
};

// Reads the index - main, file description and slice checksum packets - from
// the .par2 files given; any one with all of them will do. Damaged packets are
// skipped. False, with *error, if the set isn't complete.
bool Par2ReadSet(const std::vector<std::string> &par2Paths, Par2Set *out, std::string *error);

// The recovery slices a volume holds, by its name: "x.vol07+08.par2" holds 8.
// 0 for a name that isn't a volume's.
int Par2VolumeBlocks(const char *name);

// For each file of the set, which of the downloaded files is it: by name, or
// for one posted under another name, by its length and the MD5 of its first
// 16KB. -1 for a file that isn't among them.
void Par2MatchFiles(const Par2Set &set, const std::vector<std::string> &names, const std::vector<std::string> &paths,
                    std::vector<int> *fileForPar);

// Called as a long job goes; false stops it.
typedef bool (*Par2ProgressFn)(unsigned long long done, unsigned long long total, void *context);

// The slices that don't match the index, as numbers across the set, in
// order. paths has each file of the set's downloaded copy, or "" for one
// that wasn't found - all of whose slices are damaged; check says which
// files to read - a file with no missing articles can be taken as good.
bool Par2FindDamaged(const Par2Set &set, const std::vector<std::string> &paths, const std::vector<bool> &check,
                     std::vector<unsigned long> *damaged, Par2ProgressFn progress, void *context);

struct Par2Recovery
{
    std::string path;
    unsigned long long offset; // of its data
    unsigned long exponent;
};

// The intact recovery slices in the volumes given, for this set - each
// checked against its packet's MD5. Repeated exponents are listed once.
void Par2ListRecovery(const Par2Set &set, const std::vector<std::string> &volumePaths,
                      std::vector<Par2Recovery> *out);

enum Par2Result
{
    PAR2_REPAIRED,
    PAR2_NOT_ENOUGH,  // fewer recovery slices than damaged ones
    PAR2_FAILED,      // the files couldn't be read or written, or the result didn't check out
    PAR2_CANCELLED
};

// Works out the damaged slices from the recovery slices and every intact one,
// and writes them into the files (paths as for Par2FindDamaged; a file that
// wasn't found is made at paths[i], which mustn't then be ""). Uses at most
// about memoryBudget bytes for the slices being rebuilt, reading the files
// again for each batch that doesn't fit. Each repaired slice is checked
// against the index after.
Par2Result Par2Repair(const Par2Set &set, const std::vector<std::string> &paths,
                      const std::vector<unsigned long> &damaged, const std::vector<Par2Recovery> &recovery,
                      unsigned long long memoryBudget, Par2ProgressFn progress, void *context, std::string *error);

#endif
