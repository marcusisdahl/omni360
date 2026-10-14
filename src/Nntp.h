#ifndef NNTP_H
#define NNTP_H

#include <stddef.h> // size_t

struct Nzb;

// ---------------------------------------------------------------------------
// Downloading a post from a Usenet provider, NZBGet-style, on the console
// ---------------------------------------------------------------------------
//
// Every article of an NZB (Nzb.h) is fetched with BODY <message-id> over
// several connections to the user's provider at once, each on a thread of
// its own, decoded from yEnc (Yenc.h) and written where it goes in its file.
// TLS, as most providers want it, costs the 360's CPU most of the time, so
// the connections are spread over its cores; plain NNTP (port 119) skips
// that, where a provider still offers it.
//
// Unlike NZBGet there's no par2 repair: an article the provider no longer
// has, or one that fails its CRC, can't be made good, and the download says
// how many there were. .par2 files aren't downloaded at all.
//
// Files go into a folder as F000, F001, ... - FATX names are short, and the
// posts' are long - in the NZB's order; a file's real name stays in the Nzb.

struct NntpServer
{
    char host[256];
    unsigned short port;   // 563 for TLS, 119 plain
    bool ssl;
    char user[128];
    char pass[128];
    int connections;       // 1 to NNTP_MAX_CONNECTIONS
};

#define NNTP_MAX_CONNECTIONS 16

enum NntpResult
{
    NNTP_OK,
    NNTP_INCOMPLETE,       // articles were missing or damaged - *outMissing says how many
    NNTP_AUTH_FAILED,      // the provider refused the user name or password
    NNTP_CONNECT_FAILED,   // no connection could be made at all
    NNTP_WRITE_FAILED,     // the drive refused a write
    NNTP_CANCELLED
};

struct NntpProgress
{
    unsigned long long done;  // bytes of articles fetched, yEnc and all
    unsigned long long total; // of every article to fetch
    unsigned long missing;    // articles missing or damaged so far
    int connections;          // connected now
};

// Called about twice a second from the calling thread while the download
// runs. False stops it.
typedef bool (*NntpProgressFn)(const NntpProgress &progress, void *context);

// Downloads every file of nzb but its .par2 ones into dir, which must
// exist. A file whose name the NZB didn't give gets the one its yEnc header
// does. Blocks until done; error says what went wrong, fit for the screen.
NntpResult NntpDownload(const NntpServer &server, Nzb *nzb, const char *dir, NntpProgressFn progressFn,
                        void *context, unsigned long *outMissing, char *error, size_t errorSize);

// Where NntpDownload put file `index` of the NZB.
void NntpFilePath(const char *dir, int index, char *out, size_t outSize);

#endif
