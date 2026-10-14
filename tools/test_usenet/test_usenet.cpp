// Checks Omni360's Usenet pieces that don't need a console - the yEnc
// decoder, the NZB reader, and reading a disc image straight out of stored
// RAR volumes - against what make_test_data.py wrote.
//
//   python make_test_data.py
//   build_and_run.bat

#include "../../src/Yenc.h"
#include "../../src/Nzb.h"
#include "../../src/RarVolumes.h"
#include "../../src/Md5Digest.h"
#include "../../src/Par2.h"

#include <windows.h> // CopyFile, FindFirstFile - for the par2 test's working copy

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do                                                                           \
    {                                                                            \
        if (!(cond))                                                             \
        {                                                                        \
            printf("  FAILED line %d: %s\n", __LINE__, #cond);                   \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static std::vector<unsigned char> ReadAll(const std::string &path)
{
    std::vector<unsigned char> data;
    FILE *f = fopen(path.c_str(), "rb");
    if (f == NULL)
        return data;
    unsigned char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        data.insert(data.end(), chunk, chunk + n);
    fclose(f);
    return data;
}

static bool Exists(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (f != NULL)
        fclose(f);
    return f != NULL;
}

static void TestNames()
{
    printf("volume names\n");
    std::string base;
    int order = 0;
    CHECK(RarVolumeName("game.part01.rar", &base, &order) && base == "game" && order == 1);
    CHECK(RarVolumeName("Game.Name.PART10.RAR", &base, &order) && base == "Game.Name" && order == 10);
    CHECK(RarVolumeName("game.rar", &base, &order) && base == "game" && order == -1);
    CHECK(RarVolumeName("game.r00", &base, &order) && base == "game" && order == 0);
    CHECK(RarVolumeName("game.r99", &base, &order) && order == 99);
    CHECK(RarVolumeName("game.s00", &base, &order) && order == 100);
    CHECK(!RarVolumeName("game.iso", &base, &order));
    CHECK(!RarVolumeName("game.par2", &base, &order));
    CHECK(!RarVolumeName("game.rx1", &base, &order));
    CHECK(!RarVolumeName("rar", &base, &order));

    char name[64];
    CHECK(NzbNameFromSubject("Game [1/9] - \"game.part01.rar\" yEnc (1/50)", name, sizeof(name)) &&
          strcmp(name, "game.part01.rar") == 0);
    CHECK(!NzbNameFromSubject("no name yEnc (1/1)", name, sizeof(name)));
}

static void TestNzb(const std::string &dir)
{
    printf("NZB\n");
    std::vector<unsigned char> xml = ReadAll(dir + "\\post.nzb");
    CHECK(!xml.empty());
    xml.push_back(0);

    Nzb nzb;
    CHECK(NzbParse((const char *)&xml[0], &nzb));
    CHECK(nzb.fileCount == 3);
    if (nzb.fileCount != 3)
        return;

    const NzbFile &rar = nzb.files[0];
    CHECK(strcmp(rar.name, "game.part1.rar") == 0);
    CHECK(!rar.par2);
    CHECK(rar.segmentCount == 11); // 11 articles, the duplicate dropped
    for (unsigned long i = 0; i < rar.segmentCount; ++i)
    {
        const NzbSegment &s = nzb.segments[rar.firstSegment + i];
        CHECK(s.number == i + 1);
        char want[64];
        sprintf(want, "part%luof%lu.x7q@example.invalid", i + 1, rar.segmentCount);
        CHECK(strcmp(NzbSegmentId(nzb, s), want) == 0);
    }
    CHECK(rar.bytes == 11ULL * 102000);

    CHECK(strcmp(nzb.files[1].name, "game.vol00+01.par2") == 0);
    CHECK(nzb.files[1].par2);
    CHECK(strcmp(NzbSegmentId(nzb, nzb.segments[nzb.files[1].firstSegment]), "par.1@example.invalid") == 0);

    CHECK(nzb.files[2].name[0] == '\0'); // its yEnc header will have to say
    NzbFree(&nzb);

    Nzb empty;
    CHECK(!NzbParse("<nzb></nzb>", &empty));
}

static void TestYenc(const std::string &dir)
{
    printf("yEnc\n");
    std::vector<unsigned char> volume = ReadAll(dir + "\\new\\game.part1.rar");
    CHECK(!volume.empty());

    std::vector<unsigned char> rebuilt;
    unsigned char *out = (unsigned char *)malloc(1024 * 1024);
    for (int n = 1;; ++n)
    {
        char path[512];
        sprintf(path, "%s\\articles\\%03d.txt", dir.c_str(), n);
        if (!Exists(path))
            break;
        std::vector<unsigned char> body = ReadAll(path);
        YencPart part;
        const YencResult r = YencDecode((const char *)&body[0], (unsigned long)body.size(), out, 1024 * 1024, &part);
        CHECK(r == YENC_OK);
        CHECK(strcmp(part.name, "game.part1.rar") == 0);
        CHECK(part.fileSize == volume.size());
        if (rebuilt.size() < part.offset + part.length)
            rebuilt.resize((size_t)(part.offset + part.length));
        memcpy(&rebuilt[(size_t)part.offset], out, part.length);
    }
    CHECK(rebuilt == volume);

    // In place, as the downloader does it: the same bytes as into a buffer of
    // their own.
    {
        std::vector<unsigned char> body = ReadAll(dir + "\\articles\\002.txt");
        YencPart apart, inplace;
        CHECK(YencDecode((const char *)&body[0], (unsigned long)body.size(), out, 1024 * 1024, &apart) == YENC_OK);
        CHECK(YencDecode((const char *)&body[0], (unsigned long)body.size(), &body[0], (unsigned long)body.size(),
                         &inplace) == YENC_OK);
        CHECK(inplace.length == apart.length && inplace.offset == apart.offset &&
              memcmp(&body[0], out, apart.length) == 0 && strcmp(inplace.name, apart.name) == 0);
    }

    // One part, no =ypart, a name with spaces, crc32= rather than pcrc32=.
    std::vector<unsigned char> single = ReadAll(dir + "\\single.txt");
    YencPart part;
    CHECK(YencDecode((const char *)&single[0], (unsigned long)single.size(), out, 1024 * 1024, &part) == YENC_OK);
    CHECK(strcmp(part.name, "notes with spaces.nfo") == 0);
    CHECK(part.offset == 0 && part.length == part.fileSize && part.length == 50 * 14);

    // A changed byte is caught.
    std::vector<unsigned char> damaged = ReadAll(dir + "\\damaged.txt");
    CHECK(YencDecode((const char *)&damaged[0], (unsigned long)damaged.size(), out, 1024 * 1024, &part) ==
          YENC_CRC_MISMATCH);

    // Too small a buffer, a cut-off body, and something else entirely.
    CHECK(YencDecode((const char *)&single[0], (unsigned long)single.size(), out, 100, &part) == YENC_TOO_BIG);
    CHECK(YencDecode((const char *)&single[0], (unsigned long)single.size() / 2, out, 1024 * 1024, &part) ==
          YENC_TRUNCATED);
    CHECK(YencDecode("hello\r\n", 7, out, 1024, &part) == YENC_NOT_YENC);
    free(out);
}

// A set's volumes in order, as GameInstaller's FindImages puts them.
static std::vector<std::string> OrderedVolumes(const std::string &folder, const char *const *names, int count)
{
    std::vector<std::pair<int, std::string> > found;
    for (int i = 0; i < count; ++i)
    {
        std::string base;
        int order = 0;
        if (RarVolumeName(names[i], &base, &order))
            found.push_back(std::make_pair(order, folder + "\\" + names[i]));
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> paths;
    for (size_t i = 0; i < found.size(); ++i)
        paths.push_back(found[i].second);
    return paths;
}

static void TestImage(const char *label, const std::vector<std::string> &paths, const char *wantName,
                      const std::vector<unsigned char> &image)
{
    printf("RAR volumes: %s (%u)\n", label, (unsigned)paths.size());
    std::vector<RarImage> images;
    CHECK(RarWalkVolumes(paths, &images) == RAR_WALK_OK);
    CHECK(images.size() == 1);
    if (images.size() != 1)
        return;
    const RarImage &found = images[0];
    CHECK(found.name == wantName);
    CHECK(found.stored && !found.encrypted);
    CHECK(found.size == image.size());
    CHECK(found.spans.size() == paths.size());

    SpanSource source(paths, found.spans);
    CHECK(source.Size() == image.size());

    // All of it in odd-sized reads, so many cross from one volume into the next.
    std::vector<unsigned char> buffer(70001);
    bool same = true;
    for (unsigned long long at = 0; at < image.size(); at += buffer.size())
    {
        unsigned long take = (unsigned long)std::min<unsigned long long>(buffer.size(), image.size() - at);
        if (!source.ReadAt(at, &buffer[0], take) || memcmp(&buffer[0], &image[(size_t)at], take) != 0)
        {
            same = false;
            break;
        }
    }
    CHECK(same);

    // And out of order, the way the converter reads the file system.
    const unsigned long long spots[] = { image.size() - 10, 3 * 1024 * 1024 - 5, 0, 1024 * 1024 - 1, 7777777 };
    for (int i = 0; i < 5; ++i)
    {
        CHECK(source.ReadAt(spots[i], &buffer[0], 10));
        CHECK(memcmp(&buffer[0], &image[(size_t)spots[i]], 10) == 0);
    }
    CHECK(!source.ReadAt(image.size() - 5, &buffer[0], 10)); // past the end

    // A volume missing from the middle is noticed.
    std::vector<std::string> gap = paths;
    gap.erase(gap.begin() + 1);
    CHECK(RarWalkVolumes(gap, &images) == RAR_WALK_INCOMPLETE);
}

static void TestMd5()
{
    printf("MD5\n");
    unsigned char d[16];
    Md5Of("abc", 3, d);
    const unsigned char abc[16] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
                                   0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};
    CHECK(memcmp(d, abc, 16) == 0);
    // Across block boundaries, in pieces.
    std::string million(1000000, 'a');
    Md5 md5;
    Md5Init(&md5);
    for (size_t at = 0; at < million.size(); at += 777)
        Md5Update(&md5, million.data() + at, (unsigned long)std::min<size_t>(777, million.size() - at));
    Md5Final(&md5, d);
    const unsigned char a[16] = {0x77, 0x07, 0xd6, 0xae, 0x4e, 0x02, 0x7c, 0x70,
                                 0xee, 0xa2, 0xa9, 0x35, 0xc2, 0x29, 0x6f, 0x21};
    CHECK(memcmp(d, a, 16) == 0);
}

static std::vector<std::string> FilesIn(const std::string &dir)
{
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return names;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            names.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    return names;
}

static bool EndsWith(const std::string &s, const char *suffix)
{
    const size_t n = strlen(suffix);
    return s.size() >= n && _stricmp(s.c_str() + s.size() - n, suffix) == 0;
}

// make_par2_data.py's damaged download, repaired: par2cmdline's own files.
static void TestPar2(const std::string &dir)
{
    const std::string good = dir + "\\par2\\good", damagedDir = dir + "\\par2\\damaged", work = dir + "\\par2\\work";
    if (FilesIn(damagedDir).empty())
    {
        printf("par2: no data - run make_par2_data.py with par2.exe to test it\n");
        return;
    }
    printf("par2\n");

    // A working copy, so the test can run again.
    CreateDirectoryA(work.c_str(), NULL);
    std::vector<std::string> old = FilesIn(work);
    for (size_t i = 0; i < old.size(); ++i)
        DeleteFileA((work + "\\" + old[i]).c_str());
    std::vector<std::string> names = FilesIn(damagedDir);
    for (size_t i = 0; i < names.size(); ++i)
        CopyFileA((damagedDir + "\\" + names[i]).c_str(), (work + "\\" + names[i]).c_str(), FALSE);

    CHECK(Par2VolumeBlocks("game.vol10+10.par2") == 10);
    CHECK(Par2VolumeBlocks("Game.Name.vol127+128.PAR2") == 128);
    CHECK(Par2VolumeBlocks("game.par2") == 0);

    std::vector<std::string> index(1, work + "\\game.par2"), volumes;
    std::vector<std::string> dataNames, dataPaths;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (EndsWith(names[i], ".par2"))
        {
            if (Par2VolumeBlocks(names[i].c_str()) > 0)
                volumes.push_back(work + "\\" + names[i]);
            continue;
        }
        dataNames.push_back(names[i]);
        dataPaths.push_back(work + "\\" + names[i]);
    }

    Par2Set set;
    std::string why;
    CHECK(Par2ReadSet(index, &set, &why));
    CHECK(set.sliceSize == 65536);
    CHECK(set.files.size() == 10);
    if (set.files.size() != 10)
        return;

    // Matched by name - and the renamed one by its first 16KB.
    std::vector<int> match;
    Par2MatchFiles(set, dataNames, dataPaths, &match);
    std::vector<std::string> paths(set.files.size());
    std::vector<bool> check(set.files.size(), true);
    int unmatched = 0;
    for (size_t i = 0; i < set.files.size(); ++i)
    {
        if (match[i] < 0)
        {
            unmatched++;
            CHECK(set.files[i].name == "game.part9.rar");
            continue;
        }
        paths[i] = dataPaths[match[i]];
        if (set.files[i].name == "game.part5.rar")
            CHECK(dataNames[match[i]] == "a8f7e6d5c4b3");
        else
            CHECK(dataNames[match[i]] == set.files[i].name);
    }
    CHECK(unmatched == 1);

    // The intact copy checks out - the last slice of each file zero-padded,
    // as par2cmdline does it.
    {
        std::vector<std::string> goodPaths;
        for (size_t i = 0; i < set.files.size(); ++i)
            goodPaths.push_back(good + "\\" + set.files[i].name);
        std::vector<unsigned long> none;
        CHECK(Par2FindDamaged(set, goodPaths, check, &none, NULL, NULL) && none.empty());
    }

    std::vector<unsigned long> damaged;
    CHECK(Par2FindDamaged(set, paths, check, &damaged, NULL, NULL));
    printf("  %u damaged slice(s) of %lu\n", (unsigned)damaged.size(), set.sliceCount);
    CHECK(damaged.size() > 17 && damaged.size() < 40); // part9's 17, and a few in parts 3 and 7

    std::vector<Par2Recovery> recovery;
    Par2ListRecovery(set, volumes, &recovery);
    CHECK(recovery.size() == 40);

    // Too few: one volume's ten.
    {
        std::vector<Par2Recovery> some;
        Par2ListRecovery(set, std::vector<std::string>(1, volumes[0]), &some);
        CHECK(some.size() == 10);
        CHECK(Par2Repair(set, paths, damaged, some, 1 << 20, NULL, NULL, &why) == PAR2_NOT_ENOUGH);
    }

    // The missing file is made where par2 can fill it; three slices rebuilt
    // at a time, so the batching is tested too.
    for (size_t i = 0; i < set.files.size(); ++i)
    {
        if (paths[i].empty())
            paths[i] = work + "\\R009";
    }
    const Par2Result r = Par2Repair(set, paths, damaged, recovery, 3 * 65536, NULL, NULL, &why);
    if (r != PAR2_REPAIRED)
        printf("  repair: %s\n", why.c_str());
    CHECK(r == PAR2_REPAIRED);

    for (size_t i = 0; i < set.files.size(); ++i)
    {
        const bool same = ReadAll(paths[i]) == ReadAll(good + "\\" + set.files[i].name);
        if (!same)
            printf("  %s isn't what it was\n", set.files[i].name.c_str());
        CHECK(same);
    }
}

int main(int argc, char **argv)
{
    const std::string dir = (argc > 1) ? argv[1] : "data";
    std::vector<unsigned char> image = ReadAll(dir + "\\image.iso");
    if (image.empty())
    {
        printf("No test data in %s - run make_test_data.py first.\n", dir.c_str());
        return 2;
    }

    TestNames();
    TestMd5();
    TestNzb(dir);
    TestYenc(dir);
    TestPar2(dir);

    const char *oldNames[] = { "game.r02", "game.rar", "game.r00", "game.r01" };
    TestImage("old-style names", OrderedVolumes(dir + "\\old", oldNames, 4), "Game.iso", image);

    // Listed backwards, so part10 has to be put after part9.
    std::vector<std::string> newNames;
    for (int i = 99; i >= 1; --i)
    {
        char name[32];
        sprintf(name, "game.part%d.rar", i);
        if (Exists(dir + "\\new\\" + name))
            newNames.push_back(name);
    }
    CHECK(newNames.size() >= 10);
    std::vector<const char *> newPointers;
    for (size_t i = 0; i < newNames.size(); ++i)
        newPointers.push_back(newNames[i].c_str());
    TestImage("new-style names", OrderedVolumes(dir + "\\new", &newPointers[0], (int)newPointers.size()),
              "Game\\Game.iso", image);

    // A bare image is one slice of one file.
    printf("bare image\n");
    std::vector<std::string> one(1, dir + "\\image.iso");
    ImageSpan whole = { 0, 0, image.size() };
    SpanSource bare(one, std::vector<ImageSpan>(1, whole));
    std::vector<unsigned char> buffer(4096);
    CHECK(bare.ReadAt(image.size() - 4096, &buffer[0], 4096) &&
          memcmp(&buffer[0], &image[image.size() - 4096], 4096) == 0);

    printf(g_failures == 0 ? "\nAll passed.\n" : "\n%d check(s) FAILED.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
