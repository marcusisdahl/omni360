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
// room for capacity + 1 bytes.
static int FetchUrl(const char *url, const char *headers, char *buffer, unsigned long long capacity,
                    unsigned long long *outLen)
{
    *outLen = 0;
    std::string current = url;
    for (int hop = 0; hop < MAX_HOPS; ++hop)
    {
        if (_strnicmp(current.c_str(), "http://", 7) == 0)
        {
            // HttpGetPlain follows redirects itself.
            return HttpGetPlain(current.c_str(), headers, buffer, capacity, outLen);
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

    size_t n = strlen(out->prowlarrUrl);
    while (n > 0 && out->prowlarrUrl[n - 1] == '/')
        out->prowlarrUrl[--n] = '\0';

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
    const int status = FetchUrl(url.c_str(), headers, buffer, SEARCH_REPLY_MAX, &len);
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

char *FetchNzb(const char *nzbUrl, const char *prowlarrKey, unsigned long *outLen, char *error, size_t errorSize)
{
    *outLen = 0;
    char *buffer = (char *)malloc(NZB_MAX + 1);
    if (buffer == NULL)
    {
        SetError(error, errorSize, "Out of memory.");
        return NULL;
    }

    char headers[160];
    ApiKeyHeader(prowlarrKey, headers, sizeof(headers));
    unsigned long long len = 0;
    const int status = FetchUrl(nzbUrl, headers, buffer, NZB_MAX, &len);
    const std::string host = HostOf(nzbUrl);
    if (status != 200)
    {
        if (status <= 0)
            SetError(error, errorSize, "Couldn't reach %s for the NZB.", host.c_str());
        else if (status == 429)
            SetError(error, errorSize, "The indexer's API limit has been reached.");
        else
            SetError(error, errorSize, "Getting the NZB: HTTP %d from %s.", status, host.c_str());
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
