/*
FILE : Usenet.cpp
PROJECT : Omni360
DESCRIPTION : Searching Prowlarr, fetching the chosen NZB, and the Usenet
              settings. See Usenet.h.
*/

#include "Usenet.h"
#include "downloadFile.h"
#include "HttpPlain.h"
#include "parsing.h"     // UrlEncodeFormValue, LogEscapePercent
#include "cJSON.h"
#include "OutputConsole.h"
#include "zlib/zlib.h"   // a gzipped NZB

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>

#define SETTINGS_FILE      "game:\\settings.txt"
#define SEARCH_REPLY_MAX   (4UL * 1024 * 1024)
#define NZB_MAX            (16UL * 1024 * 1024)
#define MAX_HOPS           4
#define DEFAULT_CONNECTIONS 8
#define PROWLARR_PORT      "9696"
#define SEARCH_WAIT_MS     (120UL * 1000) // Prowlarr answers once every indexer has, or timed out
#define NZB_WAIT_MS        (90UL * 1000)  // it fetches the NZB from the indexer before sending any of it

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void SetError(char *error, size_t errorSize, const char *format, ...)
{
    if (error == NULL || errorSize == 0)
        return;
    va_list args;
    va_start(args, format);
    _vsnprintf(error, errorSize, format, args);
    va_end(args);
    error[errorSize - 1] = '\0';
}

static void CopyText(char *out, size_t outSize, const char *in)
{
    if (outSize == 0)
        return;
    _snprintf(out, outSize, "%s", in != NULL ? in : "");
    out[outSize - 1] = '\0';
}

// A string as the log can take it - see LogEscapePercent.
static std::string ForLog(const char *s)
{
    char safe[600];
    LogEscapePercent(s != NULL ? s : "", safe, sizeof(safe));
    return safe;
}

// The host of a URL, for messages.
static std::string HostOf(const char *url)
{
    const char *start = strstr(url, "://");
    start = (start != NULL) ? start + 3 : url;
    const char *end = start;
    while (*end != '\0' && *end != '/' && *end != '?')
        end++;
    return std::string(start, end - start);
}

// A GET into buffer, over plain HTTP - Prowlarr on the local network, as a
// rule - or HTTPS, following redirects. Returns the HTTP status; on 200 the
// body is in buffer, NUL-terminated, with its length in *outLen. buffer needs
// room for capacity + 1 bytes. waitMs is how long the server may take to
// start answering over plain HTTP.
static int FetchUrl(const char *url, const char *headers, char *buffer, unsigned long long capacity,
                    unsigned long long *outLen, unsigned long waitMs)
{
    *outLen = 0;
    std::string current = url;
    for (int hop = 0; hop < MAX_HOPS; ++hop)
    {
        if (_strnicmp(current.c_str(), "http://", 7) == 0)
        {
            // HttpGetPlain follows plain redirects itself; one that went on
            // to HTTPS and was redirected again hands its target back.
            const int status = HttpGetPlainWaiting(current.c_str(), headers, buffer, capacity, outLen, waitMs);
            if (status == 302 && _strnicmp(buffer, "http", 4) == 0)
            {
                current = buffer;
                headers = NULL; // a key for one host isn't for the next
                continue;
            }
            if (status == 200 && *outLen <= capacity)
                buffer[*outLen] = '\0';
            return status;
        }

        HttpsSession *session = HttpsSessionOpen(dprintf);
        if (session == NULL)
            return 0;
        unsigned long long len = capacity;
        const int status = HttpsSessionGet(session, current, headers, buffer, &len);
        HttpsSessionClose(session);

        if (status == 302)
        {
            current = std::string(buffer, (size_t)len);
            headers = NULL;
            continue;
        }
        if (status == 200)
        {
            *outLen = len;
            buffer[len] = '\0';
        }
        return status;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void UsenetNormaliseProwlarrUrl(const char *typed, char *out, size_t outSize)
{
    std::string url = typed;
    while (!url.empty() && (url[0] == ' ' || url[0] == '\t'))
        url.erase(0, 1);
    while (!url.empty() && (url[url.length() - 1] == ' ' || url[url.length() - 1] == '\t' ||
                            url[url.length() - 1] == '/'))
        url.erase(url.length() - 1);

    // "192.168.1.10" is Prowlarr on its own port, over plain HTTP.
    if (!url.empty() && url.find("://") == std::string::npos)
    {
        const bool hasPort = url.find(':') != std::string::npos;
        const bool hasPath = url.find('/') != std::string::npos;
        url = "http://" + url;
        if (!hasPort && !hasPath)
            url += ":" PROWLARR_PORT;
    }
    CopyText(out, outSize, url.c_str());
}

static bool ReadSetting(const char *line, const char *key, char *out, size_t outSize)
{
    const size_t keyLen = strlen(key);
    if (_strnicmp(line, key, keyLen) != 0)
        return false;
    const char *v = line + keyLen;
    while (*v == ' ' || *v == '\t')
        v++;
    CopyText(out, outSize, v);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    return true;
}

UsenetSetup LoadUsenetConfig(UsenetConfig *out)
{
    memset(out, 0, sizeof(*out));
    char port[16] = "", ssl[16] = "", connections[16] = "";

    FILE *f = fopen(SETTINGS_FILE, "rb");
    if (f != NULL)
    {
        char line[512];
        while (fgets(line, sizeof(line), f) != NULL)
        {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = '\0';
            if (line[0] == '#')
                continue;
            ReadSetting(line, "prowlarr-url:", out->prowlarrUrl, sizeof(out->prowlarrUrl)) ||
                ReadSetting(line, "prowlarr-key:", out->prowlarrKey, sizeof(out->prowlarrKey)) ||
                ReadSetting(line, "usenet-categories:", out->categories, sizeof(out->categories)) ||
                ReadSetting(line, "usenet-host:", out->server.host, sizeof(out->server.host)) ||
                ReadSetting(line, "usenet-port:", port, sizeof(port)) ||
                ReadSetting(line, "usenet-ssl:", ssl, sizeof(ssl)) ||
                ReadSetting(line, "usenet-user:", out->server.user, sizeof(out->server.user)) ||
                ReadSetting(line, "usenet-pass:", out->server.pass, sizeof(out->server.pass)) ||
                ReadSetting(line, "usenet-connections:", connections, sizeof(connections));
        }
        fclose(f);
    }

    if (out->prowlarrUrl[0] != '\0')
    {
        char typed[sizeof(out->prowlarrUrl)];
        CopyText(typed, sizeof(typed), out->prowlarrUrl);
        UsenetNormaliseProwlarrUrl(typed, out->prowlarrUrl, sizeof(out->prowlarrUrl));
    }

    out->server.ssl = !(_stricmp(ssl, "off") == 0 || _stricmp(ssl, "no") == 0 || strcmp(ssl, "0") == 0);
    const int p = atoi(port);
    out->server.port = (unsigned short)((p > 0 && p < 65536) ? p : (out->server.ssl ? 563 : 119));
    const int c = atoi(connections);
    out->server.connections = (c > 0) ? (c < NNTP_MAX_CONNECTIONS ? c : NNTP_MAX_CONNECTIONS) : DEFAULT_CONNECTIONS;

    if (out->prowlarrUrl[0] == '\0' || out->prowlarrKey[0] == '\0')
        return USENET_NO_PROWLARR;
    return out->server.host[0] != '\0' ? USENET_READY : USENET_NO_SERVER;
}

void UsenetSetupText(UsenetSetup setup, const char **heading, const char **detail)
{
    *heading = "Usenet isn't set up";
    if (setup == USENET_NO_PROWLARR)
        *detail = "Add prowlarr-url: and prowlarr-key: to settings.txt - see the Setup Guide.";
    else if (setup == USENET_NO_SERVER)
        *detail = "Add usenet-host:, usenet-user: and usenet-pass: to settings.txt - see the Setup Guide.";
    else
        *detail = "";
}

// ---------------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------------

// Newest first; those with no date last.
static int CompareReleases(const void *a, const void *b)
{
    const UsenetRelease *x = (const UsenetRelease *)a, *y = (const UsenetRelease *)b;
    if (x->posted[0] == '\0' || y->posted[0] == '\0')
        return (x->posted[0] == '\0') - (y->posted[0] == '\0');
    return strcmp(y->posted, x->posted);
}

// "1000,1050" as "&categories=1000&categories=1050".
static std::string CategoryParams(const char *categories)
{
    std::string out;
    const char *p = categories;
    while (*p != '\0')
    {
        while (*p == ',' || *p == ' ')
            p++;
        const char *start = p;
        while (*p >= '0' && *p <= '9')
            p++;
        if (p > start)
            out += "&categories=" + std::string(start, p - start);
        while (*p != '\0' && *p != ',')
            p++;
    }
    return out;
}

static void ApiKeyHeader(const char *key, char *out, size_t outSize)
{
    _snprintf(out, outSize, "X-Api-Key: %s\r\n", key);
    out[outSize - 1] = '\0';
}

int UsenetSearch(const UsenetConfig &config, const char *query, UsenetRelease *out, int maxOut,
                 char *error, size_t errorSize)
{
    SetError(error, errorSize, "");
    char encoded[512];
    if (!UrlEncodeFormValue(query, encoded, sizeof(encoded)))
    {
        SetError(error, errorSize, "That search is too long.");
        return -1;
    }

    char *buffer = (char *)malloc(SEARCH_REPLY_MAX + 1);
    if (buffer == NULL)
    {
        SetError(error, errorSize, "Out of memory.");
        return -1;
    }

    const std::string url = std::string(config.prowlarrUrl) + "/api/v1/search?query=" + encoded +
                            "&type=search&limit=100" + CategoryParams(config.categories);
    char headers[160];
    ApiKeyHeader(config.prowlarrKey, headers, sizeof(headers));

    unsigned long long len = 0;
    const int status = FetchUrl(url.c_str(), headers, buffer, SEARCH_REPLY_MAX, &len, SEARCH_WAIT_MS);
    int n = -1;
    if (status == 401 || status == 403)
        SetError(error, errorSize, "Prowlarr refused the API key - check prowlarr-key: in settings.txt.");
    else if (status <= 0)
        SetError(error, errorSize, "Couldn't reach Prowlarr at %s.", HostOf(config.prowlarrUrl).c_str());
    else if (status != 200)
        SetError(error, errorSize, "Prowlarr answered HTTP %d.", status);
    else
    {
        cJSON *root = cJSON_Parse(buffer);
        if (root == NULL || !cJSON_IsArray(root))
            SetError(error, errorSize, "Prowlarr's answer couldn't be read.");
        else
        {
            n = 0;
            const int total = cJSON_GetArraySize(root);
            for (int i = 0; i < total && n < maxOut; ++i)
            {
                const cJSON *item = cJSON_GetArrayItem(root, i);
                const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(item, "protocol");
                const cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
                const cJSON *link = cJSON_GetObjectItemCaseSensitive(item, "downloadUrl");
                if (!cJSON_IsString(protocol) || strcmp(protocol->valuestring, "usenet") != 0 ||
                    !cJSON_IsString(title) || !cJSON_IsString(link) || strlen(link->valuestring) >= USENET_URL_MAX)
                    continue;

                UsenetRelease &r = out[n++];
                memset(&r, 0, sizeof(r));
                CopyText(r.title, sizeof(r.title), title->valuestring);
                CopyText(r.nzbUrl, sizeof(r.nzbUrl), link->valuestring);

                const cJSON *indexer = cJSON_GetObjectItemCaseSensitive(item, "indexer");
                CopyText(r.indexer, sizeof(r.indexer), cJSON_IsString(indexer) ? indexer->valuestring : "");

                const cJSON *size = cJSON_GetObjectItemCaseSensitive(item, "size");
                r.size = cJSON_IsNumber(size) && size->valuedouble > 0 ? (unsigned long long)size->valuedouble : 0;

                const cJSON *grabs = cJSON_GetObjectItemCaseSensitive(item, "grabs");
                r.grabs = cJSON_IsNumber(grabs) ? grabs->valueint : -1;

                // "2026-10-12T08:41:00Z" - the date is enough.
                const cJSON *date = cJSON_GetObjectItemCaseSensitive(item, "publishDate");
                if (cJSON_IsString(date) && strlen(date->valuestring) >= 10)
                {
                    memcpy(r.posted, date->valuestring, 10);
                    r.posted[10] = '\0';
                }
            }
        }
        cJSON_Delete(root);
    }
    free(buffer);

    if (n > 1)
        qsort(out, n, sizeof(UsenetRelease), CompareReleases);
    dprintf("[usenet] Prowlarr search \"%s\": %d release(s)%s%s\n", ForLog(query).c_str(), n, n < 0 ? " - " : "",
            n < 0 ? ForLog(error).c_str() : "");
    return n;
}

static bool IsLoopbackHost(const std::string &hostAndPort)
{
    std::string host = hostAndPort;
    if (!host.empty() && host[0] == '[')
        host = host.substr(0, host.find(']') + 1); // [::1]:9696
    else if (host.find(':') != std::string::npos)
        host = host.substr(0, host.find(':'));
    return _stricmp(host.c_str(), "localhost") == 0 || host == "127.0.0.1" || host == "0.0.0.0" || host == "[::1]";
}

// The release's downloadUrl, made reachable from the console. Prowlarr builds
// it from the address it was asked on, so it's normally right as it is; but
// a Prowlarr behind a proxy, or set up with its own URL, can hand back a
// link to localhost, or one with no host at all.
static std::string ReachableNzbUrl(const UsenetConfig &config, const char *downloadUrl)
{
    const std::string base = config.prowlarrUrl;
    if (downloadUrl[0] == '/')
        return base + downloadUrl;

    const char *scheme = strstr(downloadUrl, "://");
    if (scheme == NULL)
        return base + "/" + downloadUrl;
    const char *hostStart = scheme + 3;
    const char *rest = hostStart;
    while (*rest != '\0' && *rest != '/' && *rest != '?')
        rest++;
    if (IsLoopbackHost(std::string(hostStart, rest - hostStart)))
    {
        // Prowlarr's own address, with whatever base path it was given.
        const char *baseHostStart = strstr(base.c_str(), "://") + 3;
        const char *basePath = strchr(baseHostStart, '/');
        const std::string origin = basePath != NULL ? base.substr(0, basePath - base.c_str()) : base;
        return origin + rest;
    }
    return downloadUrl;
}

// A gzipped NZB, in place - some indexers send them that way, whatever was
// asked for. False if it doesn't unpack into NZB_MAX.
static bool Gunzip(char **buffer, unsigned long long *len)
{
    char *out = (char *)malloc(NZB_MAX + 1);
    if (out == NULL)
        return false;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
    {
        free(out);
        return false;
    }
    zs.next_in = (Bytef *)*buffer;
    zs.avail_in = (uInt)*len;
    zs.next_out = (Bytef *)out;
    zs.avail_out = (uInt)NZB_MAX;
    const int r = inflate(&zs, Z_FINISH);
    const unsigned long outLen = zs.total_out;
    inflateEnd(&zs);
    if (r != Z_STREAM_END)
    {
        free(out);
        return false;
    }
    out[outLen] = '\0';
    free(*buffer);
    *buffer = out;
    *len = outLen;
    return true;
}

char *FetchNzb(const UsenetConfig &config, const char *downloadUrl, unsigned long *outLen, char *error,
               size_t errorSize)
{
    *outLen = 0;
    char *buffer = (char *)malloc(NZB_MAX + 1);
    if (buffer == NULL)
    {
        SetError(error, errorSize, "Out of memory.");
        return NULL;
    }

    // For a Usenet indexer, Prowlarr's link doesn't hand over the NZB: it
    // records the grab and answers 301 with the indexer's own link, and the
    // NZB comes from the indexer - over HTTPS, so its certificate has to
    // chain to a root in TrustAnchors.h. The link carries Prowlarr's key
    // itself, so no header is sent: one would follow the redirect to the
    // indexer.
    const std::string url = ReachableNzbUrl(config, downloadUrl);
    const std::string host = HostOf(url.c_str());

    unsigned long long len = 0;
    const int status = FetchUrl(url.c_str(), NULL, buffer, NZB_MAX, &len, NZB_WAIT_MS);
    if (status != 200)
    {
        if (status <= 0)
            SetError(error, errorSize, "Couldn't get the NZB from %s or the indexer it points to.", host.c_str());
        else if (status == 429)
            SetError(error, errorSize, "The indexer's download limit has been reached - see Prowlarr.");
        else if (status == 410)
            SetError(error, errorSize, "Prowlarr says the indexer is disabled, or the release is gone.");
        else if (status == 401 || status == 403)
            SetError(error, errorSize, "The NZB download was refused (HTTP %d) - check the keys in Prowlarr.", status);
        else
            SetError(error, errorSize, "Getting the NZB: HTTP %d.", status);
        free(buffer);
        return NULL;
    }

    if (len >= 2 && (unsigned char)buffer[0] == 0x1F && (unsigned char)buffer[1] == 0x8B && !Gunzip(&buffer, &len))
    {
        SetError(error, errorSize, "The NZB from %s couldn't be unpacked.", host.c_str());
        free(buffer);
        return NULL;
    }
    if (strstr(buffer, "<nzb") == NULL)
    {
        SetError(error, errorSize, "%s didn't send an NZB.", host.c_str());
        free(buffer);
        return NULL;
    }

    char *shrunk = (char *)realloc(buffer, (size_t)len + 1);
    *outLen = (unsigned long)len;
    dprintf("[usenet] NZB from %s: %lu bytes\n", host.c_str(), *outLen);
    return shrunk != NULL ? shrunk : buffer;
}

int UsenetTestProwlarr(const UsenetConfig &config, char *message, size_t messageSize)
{
    char *buffer = (char *)malloc(SEARCH_REPLY_MAX + 1);
    if (buffer == NULL)
    {
        SetError(message, messageSize, "Out of memory.");
        return -1;
    }
    char headers[160];
    ApiKeyHeader(config.prowlarrKey, headers, sizeof(headers));
    unsigned long long len = 0;
    const std::string url = std::string(config.prowlarrUrl) + "/api/v1/indexer";
    const int status = FetchUrl(url.c_str(), headers, buffer, SEARCH_REPLY_MAX, &len, 30000);

    int usenet = -1;
    if (status == 401 || status == 403)
        SetError(message, messageSize, "Prowlarr is there, but refused the API key.");
    else if (status <= 0)
        SetError(message, messageSize, "Couldn't reach Prowlarr at %s.", HostOf(config.prowlarrUrl).c_str());
    else if (status != 200)
        SetError(message, messageSize, "%s answered HTTP %d - is that Prowlarr?", HostOf(config.prowlarrUrl).c_str(),
                 status);
    else
    {
        cJSON *root = cJSON_Parse(buffer);
        if (!cJSON_IsArray(root))
            SetError(message, messageSize, "%s answered, but not as Prowlarr does.", HostOf(config.prowlarrUrl).c_str());
        else
        {
            usenet = 0;
            int other = 0;
            for (int i = 0; i < cJSON_GetArraySize(root); ++i)
            {
                const cJSON *indexer = cJSON_GetArrayItem(root, i);
                const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(indexer, "protocol");
                if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(indexer, "enable")))
                    continue;
                if (cJSON_IsString(protocol) && strcmp(protocol->valuestring, "usenet") == 0)
                    usenet++;
                else
                    other++;
            }
            if (usenet > 0)
                SetError(message, messageSize, "Connected - %d Usenet indexer%s enabled.", usenet,
                         usenet == 1 ? "" : "s");
            else
                SetError(message, messageSize,
                         "Connected, but no Usenet indexer is enabled in it%s. Add one in Prowlarr.",
                         other > 0 ? " - only torrent ones" : "");
        }
        cJSON_Delete(root);
    }
    free(buffer);
    dprintf("[usenet] Prowlarr test: %s\n", ForLog(message).c_str());
    return usenet;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

bool IsUsenetDiscImage(const char *name, unsigned long long size)
{
    const char *dot = strrchr(name, '.');
    const char *ext = (dot != NULL) ? dot + 1 : "";
    if (_stricmp(ext, "iso") == 0 || _stricmp(ext, "xiso") == 0)
        return true;
    if (size < 1000000000ULL)
        return false;

    // Big, and not something that's plainly not a disc image.
    static const char *const kNot[] = { "rar", "zip", "7z", "par2", "nzb", "nfo", "sfv", "mkv", "mp4", "avi" };
    for (int i = 0; i < (int)(sizeof(kNot) / sizeof(kNot[0])); ++i)
    {
        if (_stricmp(ext, kNot[i]) == 0)
            return false;
    }
    // A split archive's ".r00" or ".001".
    if (strlen(ext) == 3 && ext[1] >= '0' && ext[1] <= '9' && ext[2] >= '0' && ext[2] <= '9' &&
        (ext[0] == 'r' || ext[0] == 'R' || ext[0] == 's' || ext[0] == 'S' || (ext[0] >= '0' && ext[0] <= '9')))
        return false;
    return true;
}

void UsenetDisplayName(const char *releaseTitle, char *out, size_t outSize)
{
    std::string name = releaseTitle;

    // "-GROUP" off the end: a last dash with a short run after it, no spaces.
    const size_t dash = name.rfind('-');
    if (dash != std::string::npos && dash > 0 && name.length() - dash <= 16 &&
        name.find(' ', dash) == std::string::npos)
        name.erase(dash);

    std::string spaced;
    for (size_t i = 0; i < name.length(); ++i)
    {
        const char c = (name[i] == '.' || name[i] == '_') ? ' ' : name[i];
        if (c == ' ' && (spaced.empty() || spaced[spaced.length() - 1] == ' '))
            continue;
        spaced += c;
    }
    while (!spaced.empty() && spaced[spaced.length() - 1] == ' ')
        spaced.erase(spaced.length() - 1);
    CopyText(out, outSize, spaced.empty() ? releaseTitle : spaced.c_str());
}
