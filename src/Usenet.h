#ifndef USENET_H
#define USENET_H

#include <stddef.h> // size_t
#include "Nntp.h"   // NntpServer

// ---------------------------------------------------------------------------
// Usenet: searching Prowlarr, and the settings for the downloader
// ---------------------------------------------------------------------------
//
// A second way to get a disc image onto the console, beside archive.org:
// Prowlarr - usually on a PC on the local network - is searched for what the
// user types, the release they pick is fetched as an NZB through it, and the
// console downloads the post itself from their Usenet provider (Nntp.h).
// Then it's installed as Games on Demand like any other game
// (GameInstaller.cpp).
//
// Everything is configured in game:\settings.txt, since keys are no fun to
// type with a controller:
//
//   prowlarr-url: http://192.168.1.10:9696
//   prowlarr-key: <Prowlarr's API key, from Settings > General>
//   usenet-categories: 1000,1050   (optional: Newznab category IDs)
//   usenet-host: news.example.com
//   usenet-port: 563               (optional: 563 with TLS, 119 without)
//   usenet-ssl: on                 (optional: "off" for plain NNTP)
//   usenet-user: <provider user name>
//   usenet-pass: <provider password>
//   usenet-connections: 8          (optional: up to the provider's limit, 16 at most)
//
// The calls here block, and are made from worker threads: searches from
// SearchWorker's, the NZB from the game installer's.

#define USENET_URL_MAX     2048 // Prowlarr's links carry the indexer's, encrypted
#define USENET_TITLE_MAX   256
#define USENET_KEY_MAX     96
#define MAX_USENET_RESULTS 60

struct UsenetConfig
{
    char prowlarrUrl[256];
    char prowlarrKey[USENET_KEY_MAX];
    char categories[96]; // comma-separated, or empty for every category
    NntpServer server;
};

enum UsenetSetup
{
    USENET_READY,
    USENET_NO_PROWLARR, // no prowlarr-url or prowlarr-key
    USENET_NO_SERVER    // no usenet-host
};

// Reads the keys above from settings.txt. Cheap enough to call per search.
UsenetSetup LoadUsenetConfig(UsenetConfig *out);

// What a setup problem means, for a toast: a heading and a line under it.
void UsenetSetupText(UsenetSetup setup, const char **heading, const char **detail);

struct UsenetRelease
{
    char title[USENET_TITLE_MAX];
    char indexer[64];
    char nzbUrl[USENET_URL_MAX];
    char posted[24];         // "2026-10-12", or empty when not known
    unsigned long long size; // 0 when not known
    int grabs;               // -1 when not known
};

// Searches Prowlarr for query - Usenet releases only. Returns how many were
// written, newest first, or -1 if Prowlarr couldn't be reached or refused
// the search; error then says why, in a line fit for the screen.
int UsenetSearch(const UsenetConfig &config, const char *query, UsenetRelease *out, int maxOut,
                 char *error, size_t errorSize);

// The NZB a release's downloadUrl points at: malloc'd and NUL-terminated,
// for the caller to free. NULL on failure, with error saying why. The link is
// Prowlarr's, and for a Usenet indexer Prowlarr redirects it to the indexer's
// own, which is followed - so the NZB comes from the indexer, over HTTPS. A
// link to localhost, or with no host, is pointed at prowlarr-url's first; a
// gzipped NZB is unpacked.
char *FetchNzb(const UsenetConfig &config, const char *downloadUrl, unsigned long *outLen, char *error,
               size_t errorSize);

// Asks Prowlarr for its indexers, for Settings' Test: how many enabled
// Usenet ones it has, or -1 if it couldn't be reached or refused the key.
// message says which, fit for the screen.
int UsenetTestProwlarr(const UsenetConfig &config, char *message, size_t messageSize);

// What's typed for Prowlarr's address, as a URL: "192.168.1.10" becomes
// "http://192.168.1.10:9696"; one with a scheme or port is kept, without a
// trailing slash.
void UsenetNormaliseProwlarrUrl(const char *typed, char *out, size_t outSize);

// Whether a downloaded file is a disc image to install as it is: an .iso, or
// anything a gigabyte or more that isn't an archive, par2 or video file.
bool IsUsenetDiscImage(const char *name, unsigned long long size);

// A release's title as a game's name, for when the disc's title ID isn't in
// the Store's lists: dots and underscores to spaces, and the group's tag
// ("-GROUP") off the end.
void UsenetDisplayName(const char *releaseTitle, char *out, size_t outSize);

#endif
