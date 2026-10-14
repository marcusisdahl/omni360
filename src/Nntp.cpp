/*
FILE : Nntp.cpp
PROJECT : Omni360
DESCRIPTION : Downloading an NZB's articles over several NNTP connections.
              See Nntp.h.
*/

#include "Nntp.h"
#include "Nzb.h"
#include "Yenc.h"
#include "XboxTLS.h"
#include "dns.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <vector>

// downloadFile.cpp's - the same roots every HTTPS connection trusts.
int addTrustAnchors(XboxTLSContext *ctx);

#define XBOX_SO_BYPASS_SECURITY 0x5801
#define IO_TIMEOUT_MS        30000 // an article starts in well under a second; 30s of nothing is a dead connection
#define READ_CHUNK           65536
#define LINE_MAX_LEN         8192  // yEnc lines are 128-256 bytes
#define BODY_START           (2UL * 1024 * 1024)
#define BODY_MAX             (32UL * 1024 * 1024)
#define ARTICLE_TRIES        3     // a server that answers but fails mid-article, before calling it missing
#define RECONNECT_TRIES      5
#define CONNECT_STAGGER_MS   250
#define PROGRESS_MS          500

// ---------------------------------------------------------------------------
// One connection
// ---------------------------------------------------------------------------

struct Conn
{
    bool ssl;
    bool open;
    SOCKET sock;
    XboxTLSContext tls;
    char buf[READ_CHUNK];
    int len;
    int pos;
};

enum ConnectResult
{
    CONNECT_OK,
    CONNECT_FAILED,  // couldn't get through, or the server turned the connection away
    CONNECT_AUTH     // the server refused the user name or password
};

static void Close(Conn *c)
{
    if (!c->open)
        return;
    if (c->ssl)
        XboxTLS_Free(&c->tls); // closes the socket too
    else
        closesocket(c->sock);
    c->open = false;
}

static int RawRead(Conn *c, char *dst, int n)
{
    return c->ssl ? XboxTLS_Read(&c->tls, dst, n) : recv(c->sock, dst, n, 0);
}

static bool WriteAll(Conn *c, const char *data, int n)
{
    if (c->ssl)
        return XboxTLS_Write(&c->tls, data, n) == n;
    while (n > 0)
    {
        const int sent = send(c->sock, data, n, 0);
        if (sent <= 0)
            return false;
        data += sent;
        n -= sent;
    }
    return true;
}

// One line, without its CRLF, into line (cap bytes, NUL-terminated). Its
// length, or -1 if the connection failed or the line didn't fit.
static int ReadLine(Conn *c, char *line, int cap)
{
    int n = 0;
    for (;;)
    {
        if (c->pos >= c->len)
        {
            const int got = RawRead(c, c->buf, READ_CHUNK);
            if (got <= 0)
                return -1;
            c->len = got;
            c->pos = 0;
        }
        const char *start = c->buf + c->pos;
        const int avail = c->len - c->pos;
        const char *nl = (const char *)memchr(start, '\n', avail);
        const int take = (nl != NULL) ? (int)(nl - start) : avail;
        if (n + take >= cap)
            return -1;
        memcpy(line + n, start, take);
        n += take;
        c->pos += take + (nl != NULL ? 1 : 0);
        if (nl != NULL)
        {
            if (n > 0 && line[n - 1] == '\r')
                n--;
            line[n] = '\0';
            return n;
        }
    }
}

static int SendCommand(Conn *c, const char *command, char *reply, int replyCap)
{
    if (!WriteAll(c, command, (int)strlen(command)))
        return -1;
    if (ReadLine(c, reply, replyCap) < 3)
        return -1;
    return atoi(reply);
}

static ConnectResult Connect(Conn *c, const NntpServer &server, char *why, size_t whySize)
{
    c->ssl = server.ssl;
    c->open = false;
    c->len = c->pos = 0;
    why[0] = '\0';

    char ip[64] = "";
    for (int attempt = 0; attempt < 3 && ip[0] == '\0'; ++attempt)
    {
        if (ResolveDNS(server.host, ip, sizeof(ip)) || searchDnsCache(server.host, ip, sizeof(ip)) == 0)
            break;
        ip[0] = '\0';
        Sleep(1000);
    }
    if (ip[0] == '\0')
    {
        _snprintf(why, whySize, "Couldn't find %s.", server.host);
        why[whySize - 1] = '\0';
        return CONNECT_FAILED;
    }

    if (c->ssl)
    {
        if (!XboxTLS_CreateContext(&c->tls, server.host))
            return CONNECT_FAILED;
        if (addTrustAnchors(&c->tls) != EXIT_SUCCESS || !XboxTLS_Connect(&c->tls, ip, server.host, server.port))
        {
            XboxTLS_Free(&c->tls);
            _snprintf(why, whySize, "Couldn't make a secure connection to %s:%u.", server.host, (unsigned)server.port);
            why[whySize - 1] = '\0';
            return CONNECT_FAILED;
        }
    }
    else
    {
        c->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (c->sock == INVALID_SOCKET)
            return CONNECT_FAILED;
        BOOL yes = TRUE;
        DWORD timeoutMs = IO_TIMEOUT_MS;
        int rcvbuf = 1024 * 1024;
        setsockopt(c->sock, SOL_SOCKET, XBOX_SO_BYPASS_SECURITY, (PCSTR)&yes, sizeof(yes));
        setsockopt(c->sock, SOL_SOCKET, SO_RCVBUF, (PCSTR)&rcvbuf, sizeof(rcvbuf));
        setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, (PCSTR)&timeoutMs, sizeof(timeoutMs));
        setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, (PCSTR)&timeoutMs, sizeof(timeoutMs));
        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(server.port);
        sa.sin_addr.s_addr = inet_addr(ip);
        if (connect(c->sock, (sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR)
        {
            closesocket(c->sock);
            _snprintf(why, whySize, "Couldn't connect to %s:%u.", server.host, (unsigned)server.port);
            why[whySize - 1] = '\0';
            return CONNECT_FAILED;
        }
    }
    c->open = true;

    char reply[512];
    if (ReadLine(c, reply, sizeof(reply)) < 3 || (atoi(reply) != 200 && atoi(reply) != 201))
    {
        // 502 is usually "too many connections" - the provider's limit.
        _snprintf(why, whySize, "%s turned the connection away: %.80s", server.host, reply);
        why[whySize - 1] = '\0';
        Close(c);
        return CONNECT_FAILED;
    }

    if (server.user[0] != '\0')
    {
        char command[300];
        _snprintf(command, sizeof(command), "AUTHINFO USER %s\r\n", server.user);
        command[sizeof(command) - 1] = '\0';
        int code = SendCommand(c, command, reply, sizeof(reply));
        if (code == 381)
        {
            _snprintf(command, sizeof(command), "AUTHINFO PASS %s\r\n", server.pass);
            command[sizeof(command) - 1] = '\0';
            code = SendCommand(c, command, reply, sizeof(reply));
        }
        if (code != 281)
        {
            Close(c);
            if (code == 481 || code == 482 || code == 452)
            {
                _snprintf(why, whySize, "%s refused the user name or password.", server.host);
                why[whySize - 1] = '\0';
                return CONNECT_AUTH;
            }
            // 502 here too can be a connection limit rather than bad details.
            _snprintf(why, whySize, "%s didn't log in: %.80s", server.host, code > 0 ? reply : "no answer");
            why[whySize - 1] = '\0';
            return CONNECT_FAILED;
        }
    }
    return CONNECT_OK;
}

// ---------------------------------------------------------------------------
// The download
// ---------------------------------------------------------------------------

struct Shared
{
    const NntpServer *server;
    Nzb *nzb;
    const char *dir;

    CRITICAL_SECTION lock;      // the fields below
    CRITICAL_SECTION writeLock; // the files: one write at a time
    std::vector<unsigned long> work;  // segment indexes, in file order
    std::vector<unsigned long> again; // taken by a connection that then lost its server - first out next
    std::vector<int> segmentFile;     // each segment's file
    size_t next;
    FILE **files;
    unsigned long *left;              // each file's segments not yet done
    unsigned long long done;
    unsigned long long total;
    unsigned long missing;
    int connected;
    int everConnected;
    bool stop;
    bool authFailed;
    bool writeFailed;
    char why[160];                    // the last connection failure
};

struct ThreadArgs
{
    Shared *s;
    int slot;
};

static bool Stopping(Shared *s)
{
    EnterCriticalSection(&s->lock);
    const bool stop = s->stop;
    LeaveCriticalSection(&s->lock);
    return stop;
}

static void NoteFailure(Shared *s, const char *why, bool auth)
{
    EnterCriticalSection(&s->lock);
    if (why[0] != '\0')
    {
        strncpy(s->why, why, sizeof(s->why) - 1);
        s->why[sizeof(s->why) - 1] = '\0';
    }
    if (auth)
    {
        s->authFailed = true;
        s->stop = true;
    }
    LeaveCriticalSection(&s->lock);
}

// Connects, trying a few times. False if it couldn't, or shouldn't go on.
static bool Reconnect(Shared *s, Conn *c, int slot)
{
    for (int attempt = 0; attempt < RECONNECT_TRIES && !Stopping(s); ++attempt)
    {
        if (attempt > 0)
            Sleep(2000 * attempt);
        char why[160];
        const ConnectResult r = Connect(c, *s->server, why, sizeof(why));
        if (r == CONNECT_OK)
            return true;
        dprintf("[nntp] connection %d: %s\n", slot + 1, why);
        NoteFailure(s, why, r == CONNECT_AUTH);
        if (r == CONNECT_AUTH)
            return false;
    }
    return false;
}

// The decoded part, written where it goes in its file.
static bool WritePart(Shared *s, int fileIndex, const YencPart &part, const unsigned char *data)
{
    bool ok = false;
    EnterCriticalSection(&s->writeLock);
    FILE *&f = s->files[fileIndex];
    if (f == NULL)
    {
        char path[300];
        NntpFilePath(s->dir, fileIndex, path, sizeof(path));
        f = fopen(path, "r+b");
        if (f == NULL)
            f = fopen(path, "w+b");
    }
    if (f != NULL)
        ok = _fseeki64(f, (__int64)part.offset, SEEK_SET) == 0 && fwrite(data, 1, part.length, f) == part.length;
    LeaveCriticalSection(&s->writeLock);
    return ok;
}

static void FinishSegment(Shared *s, unsigned long index, bool good)
{
    const int file = s->segmentFile[index];
    bool closeIt = false;
    EnterCriticalSection(&s->lock);
    s->done += s->nzb->segments[index].bytes;
    if (!good)
        s->missing++;
    closeIt = (--s->left[file] == 0);
    LeaveCriticalSection(&s->lock);

    if (closeIt)
    {
        EnterCriticalSection(&s->writeLock);
        if (s->files[file] != NULL)
        {
            if (fclose(s->files[file]) != 0)
            {
                EnterCriticalSection(&s->lock);
                s->writeFailed = true;
                s->stop = true;
                LeaveCriticalSection(&s->lock);
            }
            s->files[file] = NULL;
        }
        LeaveCriticalSection(&s->writeLock);
    }
}

static DWORD WINAPI ConnectionEntry(LPVOID param)
{
    ThreadArgs *args = (ThreadArgs *)param;
    Shared *s = args->s;
    const int slot = args->slot;

    XboxTLS_SetThreadIoTimeout(IO_TIMEOUT_MS);

    Conn *c = (Conn *)malloc(sizeof(Conn));
    unsigned long bodyCap = BODY_START;
    char *body = (char *)malloc(bodyCap);
    unsigned char *decoded = (unsigned char *)malloc(bodyCap);
    char *line = (char *)malloc(LINE_MAX_LEN);
    if (c == NULL || body == NULL || decoded == NULL || line == NULL)
    {
        free(c);
        free(body);
        free(decoded);
        free(line);
        return 0;
    }
    memset(c, 0, sizeof(*c));

    if (Reconnect(s, c, slot))
    {
        EnterCriticalSection(&s->lock);
        s->connected++;
        s->everConnected++;
        LeaveCriticalSection(&s->lock);

        unsigned long articles = 0;
        for (;;)
        {
            unsigned long index = 0;
            EnterCriticalSection(&s->lock);
            const bool haveWork = !s->stop && (!s->again.empty() || s->next < s->work.size());
            if (haveWork)
            {
                if (!s->again.empty())
                {
                    index = s->again.back();
                    s->again.pop_back();
                }
                else
                    index = s->work[s->next++];
            }
            LeaveCriticalSection(&s->lock);
            if (!haveWork)
                break;

            const NzbSegment &segment = s->nzb->segments[index];
            char command[600];
            _snprintf(command, sizeof(command), "BODY <%s>\r\n", NzbSegmentId(*s->nzb, segment));
            command[sizeof(command) - 1] = '\0';

            bool got = false, gone = false, lost = false;
            unsigned long bodyLen = 0;
            for (int attempt = 0; attempt < ARTICLE_TRIES && !got && !gone && !lost; ++attempt)
            {
                if (!c->open && !Reconnect(s, c, slot))
                {
                    lost = true;
                    break;
                }

                const int code = SendCommand(c, command, line, LINE_MAX_LEN);
                if (code == 430 || code == 423)
                {
                    gone = true; // the provider hasn't got it
                    break;
                }
                if (code != 222)
                {
                    // Cut off, or a 4xx/5xx that means the connection's done.
                    Close(c);
                    continue;
                }

                // The body, dot-stuffing undone, up to its "." line.
                bodyLen = 0;
                bool complete = false, failed = false;
                for (;;)
                {
                    const int n = ReadLine(c, line, LINE_MAX_LEN);
                    if (n < 0)
                    {
                        failed = true;
                        break;
                    }
                    if (n == 1 && line[0] == '.')
                    {
                        complete = true;
                        break;
                    }
                    const char *text = (n >= 2 && line[0] == '.' && line[1] == '.') ? line + 1 : line;
                    const unsigned long textLen = (unsigned long)(n - (text - line));
                    if (bodyLen + textLen + 1 > bodyCap)
                    {
                        unsigned long newCap = bodyCap * 2;
                        char *bigger = (newCap <= BODY_MAX) ? (char *)realloc(body, newCap) : NULL;
                        unsigned char *biggerOut = (bigger != NULL) ? (unsigned char *)realloc(decoded, newCap) : NULL;
                        if (bigger != NULL)
                            body = bigger;
                        if (biggerOut != NULL)
                        {
                            decoded = biggerOut;
                            bodyCap = newCap;
                        }
                        else
                        {
                            failed = true; // keeps reading would desync the connection
                            break;
                        }
                    }
                    memcpy(body + bodyLen, text, textLen);
                    bodyLen += textLen;
                    body[bodyLen++] = '\n';
                }
                if (complete)
                    got = true;
                else
                    Close(c);
            }

            if (lost)
            {
                // Back for another connection, and this one stops.
                EnterCriticalSection(&s->lock);
                s->again.push_back(index);
                LeaveCriticalSection(&s->lock);
                break;
            }

            bool good = false;
            if (got)
            {
                YencPart part;
                const YencResult r = YencDecode(body, bodyLen, decoded, bodyCap, &part);
                if (r == YENC_OK || r == YENC_CRC_MISMATCH)
                {
                    const int file = s->segmentFile[index];
                    NzbFile &nzbFile = s->nzb->files[file];
                    if (nzbFile.name[0] == '\0' && part.name[0] != '\0')
                    {
                        EnterCriticalSection(&s->lock);
                        if (nzbFile.name[0] == '\0')
                        {
                            strncpy(nzbFile.name, part.name, sizeof(nzbFile.name) - 1);
                            nzbFile.name[sizeof(nzbFile.name) - 1] = '\0';
                        }
                        LeaveCriticalSection(&s->lock);
                    }
                    if (!WritePart(s, file, part, decoded))
                    {
                        EnterCriticalSection(&s->lock);
                        s->writeFailed = true;
                        s->stop = true;
                        LeaveCriticalSection(&s->lock);
                    }
                    good = (r == YENC_OK);
                }
                if (!good)
                    dprintf("[nntp] article %lu of file %d: %s\n", segment.number, s->segmentFile[index] + 1,
                            r == YENC_CRC_MISMATCH ? "damaged (CRC)" : r == YENC_TOO_BIG ? "too big" : "not yEnc");
            }
            else
                dprintf("[nntp] article %lu of file %d: not on the server\n", segment.number,
                        s->segmentFile[index] + 1);
            FinishSegment(s, index, good);
            articles++;
        }

        if (c->open)
        {
            char reply[128];
            SendCommand(c, "QUIT\r\n", reply, sizeof(reply));
        }
        Close(c);
        EnterCriticalSection(&s->lock);
        s->connected--;
        LeaveCriticalSection(&s->lock);
        dprintf("[nntp] connection %d done: %lu article(s)\n", slot + 1, articles);
    }

    free(c);
    free(body);
    free(decoded);
    free(line);
    return 0;
}

bool NntpTestLogin(const NntpServer &server, char *message, size_t messageSize)
{
    XNetStartupParams xnsp;
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    XNetStartup(&xnsp);
    WSADATA wsadata;
    WSAStartup(MAKEWORD(2, 2), &wsadata);
    XboxTLS_SetThreadIoTimeout(15000);

    Conn *c = (Conn *)malloc(sizeof(Conn));
    bool ok = false;
    if (c != NULL)
    {
        memset(c, 0, sizeof(*c));
        char why[160];
        const ConnectResult r = Connect(c, server, why, sizeof(why));
        ok = (r == CONNECT_OK);
        if (ok)
        {
            char reply[128];
            SendCommand(c, "QUIT\r\n", reply, sizeof(reply));
            Close(c);
            _snprintf(message, messageSize, "Logged in to %s%s.", server.host,
                      server.ssl ? " over TLS" : " (not encrypted)");
        }
        else
            _snprintf(message, messageSize, "%s", why[0] != '\0' ? why : "Couldn't connect.");
        message[messageSize - 1] = '\0';
        free(c);
    }
    XboxTLS_SetThreadIoTimeout(0);
    WSACleanup();
    XNetCleanup();
    dprintf("[nntp] login test: %s\n", ok ? "ok" : "failed");
    return ok;
}

void NntpFilePath(const char *dir, int index, char *out, size_t outSize)
{
    _snprintf(out, outSize, "%s\\F%03d", dir, index);
    out[outSize - 1] = '\0';
}

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

NntpResult NntpDownload(const NntpServer &server, Nzb *nzb, const char *dir, NntpProgressFn progressFn,
                        void *context, unsigned long *outMissing, char *error, size_t errorSize)
{
    *outMissing = 0;
    SetError(error, errorSize, "");

    Shared *s = new Shared;
    s->server = &server;
    s->nzb = nzb;
    s->dir = dir;
    s->next = 0;
    s->done = 0;
    s->total = 0;
    s->missing = 0;
    s->connected = 0;
    s->everConnected = 0;
    s->stop = false;
    s->authFailed = false;
    s->writeFailed = false;
    s->why[0] = '\0';
    s->files = (FILE **)calloc(nzb->fileCount, sizeof(FILE *));
    s->left = (unsigned long *)calloc(nzb->fileCount, sizeof(unsigned long));
    s->segmentFile.assign(nzb->segmentCount, -1);
    for (int f = 0; f < nzb->fileCount; ++f)
    {
        const NzbFile &file = nzb->files[f];
        for (unsigned long i = 0; i < file.segmentCount; ++i)
        {
            s->segmentFile[file.firstSegment + i] = f;
            if (!file.par2)
                s->work.push_back(file.firstSegment + i);
        }
        if (!file.par2)
        {
            s->left[f] = file.segmentCount;
            s->total += file.bytes;
        }
    }
    InitializeCriticalSection(&s->lock);
    InitializeCriticalSection(&s->writeLock);

    int connections = server.connections;
    if (connections < 1)
        connections = 1;
    if (connections > NNTP_MAX_CONNECTIONS)
        connections = NNTP_MAX_CONNECTIONS;
    if ((size_t)connections > s->work.size())
        connections = (int)s->work.size();

    dprintf("[nntp] %d file(s), %lu article(s), %I64u MB from %s:%u (%s), %d connection(s)\n", nzb->fileCount,
            (unsigned long)s->work.size(), s->total / (1024 * 1024), server.host, (unsigned)server.port,
            server.ssl ? "TLS" : "plain", connections);

    // Reference-counted - held for the download, like an HttpsSession.
    XNetStartupParams xnsp;
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    XNetStartup(&xnsp);
    WSADATA wsadata;
    WSAStartup(MAKEWORD(2, 2), &wsadata);

    ThreadArgs args[NNTP_MAX_CONNECTIONS];
    HANDLE threads[NNTP_MAX_CONNECTIONS];
    int started = 0;
    const DWORD begun = GetTickCount();
    bool cancelled = false;
    for (int i = 0; i < connections; ++i)
    {
        args[i].s = s;
        args[i].slot = i;
        threads[i] = CreateThread(NULL, 256 * 1024, ConnectionEntry, &args[i], CREATE_SUSPENDED, NULL);
        if (threads[i] == NULL)
            break;
#ifdef _XBOX
        // TLS decryption is most of a connection's work: around the cores,
        // away from the UI's hardware thread 0. Thread 3 is the installer's,
        // which only waits while this runs.
        static const DWORD kProcessors[] = { 1, 4, 5, 2 };
        XSetThreadProcessor(threads[i], kProcessors[i % 4]);
#endif
        ResumeThread(threads[i]);
        started++;
        Sleep(CONNECT_STAGGER_MS);
    }

    // Report until every connection has finished.
    while (started > 0)
    {
        const DWORD waited = WaitForMultipleObjects(started, threads, TRUE, PROGRESS_MS);
        NntpProgress progress;
        EnterCriticalSection(&s->lock);
        progress.done = s->done;
        progress.total = s->total;
        progress.missing = s->missing;
        progress.connections = s->connected;
        LeaveCriticalSection(&s->lock);
        if (!cancelled && progressFn != NULL && !progressFn(progress, context))
        {
            cancelled = true;
            EnterCriticalSection(&s->lock);
            s->stop = true;
            LeaveCriticalSection(&s->lock);
        }
        if (waited != WAIT_TIMEOUT)
            break;
    }
    for (int i = 0; i < started; ++i)
        CloseHandle(threads[i]);

    for (int f = 0; f < nzb->fileCount; ++f)
    {
        if (s->files[f] != NULL && fclose(s->files[f]) != 0)
            s->writeFailed = true;
    }

    const bool unfinished = !s->again.empty() || s->next < s->work.size();
    const DWORD seconds = (GetTickCount() - begun) / 1000;
    dprintf("[nntp] %I64u MB in %lu:%02lu (%.2f MB/s), %lu article(s) missing%s\n", s->done / (1024 * 1024),
            (unsigned long)(seconds / 60), (unsigned long)(seconds % 60),
            seconds > 0 ? s->done / 1048576.0 / seconds : 0.0, s->missing, unfinished ? ", unfinished" : "");

    NntpResult result = NNTP_OK;
    if (cancelled)
        result = NNTP_CANCELLED;
    else if (s->authFailed)
    {
        result = NNTP_AUTH_FAILED;
        SetError(error, errorSize, "%s", s->why);
    }
    else if (s->writeFailed)
    {
        result = NNTP_WRITE_FAILED;
        SetError(error, errorSize, "A file couldn't be written - is the drive full?");
    }
    else if (unfinished)
    {
        result = NNTP_CONNECT_FAILED;
        if (s->everConnected == 0)
            SetError(error, errorSize, "%s", s->why[0] != '\0' ? s->why : "Couldn't connect to the provider.");
        else
            SetError(error, errorSize, "Lost the connection to %s.", server.host);
    }
    else if (s->missing > 0)
    {
        result = NNTP_INCOMPLETE;
        SetError(error, errorSize, "%lu article(s) were missing or damaged, and there's no par2 repair.",
                 s->missing);
    }
    *outMissing = s->missing;

    WSACleanup();
    XNetCleanup();
    DeleteCriticalSection(&s->lock);
    DeleteCriticalSection(&s->writeLock);
    free(s->files);
    free(s->left);
    delete s;
    return result;
}
