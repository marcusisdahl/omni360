# Usenet

The Store's **Usenet** button is a second way to get a disc image onto the console, beside archive.org. Prowlarr is searched for what the user types, the release they pick is fetched as an NZB through it, and the console downloads the post itself from their Usenet provider: an NZBGet-style downloader, built in. Then the disc image in it is installed as Games on Demand by the same code an archive.org game goes through.

Setting it up is in the [Setup Guide](Setup%20Guide.md#7-optional-usenet). This is how it works.

## The flow

```text
Store → Usenet → keyboard
  → SearchWorker (SEARCH_USENET)
       UsenetSearch()          GET {prowlarr-url}/api/v1/search?query=…&type=search   X-Api-Key
  → the picker (PICKER_USENET): title, size, indexer, date, grabs
  → A: QueueUsenetRelease() → EnqueueGameInstall(GameRequest{ usenet = true, nzbUrl })
GameInstaller's worker: RunJob → RunUsenetJob
  1. LoadUsenetConfig()         settings.txt, read when the job starts
  2. FetchNzb()                 the release's downloadUrl: Prowlarr answers 301 to the indexer's link,
                                and the NZB comes from the indexer, over HTTPS
  3. NzbParse()                 files, segments, message IDs
  4. room check                 the download (par2 volumes left out) and the game: twice the download
  5. NntpDownload()             every file but the par2 volumes into <drive>\Omni360Staging\U<job id>\F000, F001, …
                                - stopping early if more goes missing than the volumes could repair
  5b. RepairWithPar2()          names files from the par2 index, checks those that lacked articles,
                                fetches just enough volumes, repairs (Par2.cpp)
  6. FindImages()               bare .iso files, and disc images in stored RAR sets
  7. InstallImage() per image   GodInspect → room check → GodConvert, through a SpanSource
  8. the staging folder goes, whatever happened
```

The job is a game job like any other: one at a time on the installer's worker, listed on the Queue page, stopped by **X**, reported by a popup.

## Modules

| File | What it does | Xbox-free? |
|---|---|---|
| `Usenet.cpp` / `.h` | The `settings.txt` keys, the Prowlarr search, fetching the NZB, the display name from a release title | No (HTTP) |
| `Nzb.cpp` / `.h` | Reads an NZB: each `<file>`'s name (the quoted part of its subject), its segments in order with duplicates dropped, their message IDs, and whether it's a `.par2` | Yes |
| `Yenc.cpp` / `.h` | Decodes one article body: `=ybegin`, `=ypart`, `=yend`, escapes, and checks `pcrc32` | Yes |
| `Nntp.cpp` / `.h` | The downloader: connections, `AUTHINFO`, `BODY <id>`, dot-unstuffing, writing each part where it goes | No (sockets, TLS) |
| `RarVolumes.cpp` / `.h` | Orders a RAR set by its names, walks each volume's headers, and reads a stored file's slices across the volumes as one image (`SpanSource`) | Yes |

The Xbox-free ones are tested on a PC by `tools/test_usenet` (`python make_test_data.py`, then `build_and_run.bat`): yEnc round trips, a CRC that catches a changed byte, an NZB with segments out of order and listed twice, and a 10MB image read out of hand-made RAR4 volume sets with both naming schemes, including reads that cross volumes and a volume missing from the middle.

## Settings and Prowlarr

The keys live in `settings.txt` and can be set there or on the console, under **Settings → Usenet** (`main.cpp`, `ChangeProwlarrAddress` and the functions after it), which writes them back with `SetSettingsValue`. A typed Prowlarr address is made a URL by `UsenetNormaliseProwlarrUrl`: `192.168.1.10` becomes `http://192.168.1.10:9696`. Each search and each job reads the file again, so a change counts at once.

Two **Test** buttons check the setup without a download: Prowlarr's is `GET /api/v1/indexer`, counting enabled indexers whose `protocol` is `usenet`; the server's is one connection, logged in and closed (`NntpTestLogin`).

What Prowlarr does with a search and a grab, from its source (`SearchController`, `NewznabController.GetDownload`, `DownloadMappingService`):

- `GET /api/v1/search?query=…&type=search&limit=100[&categories=N…]` with `X-Api-Key` returns releases; each `downloadUrl` is Prowlarr's own proxy link, `{scheme}://{host}:{port}{urlBase}/{indexerId}/download?apikey=…&link=…&file=…`, built from the address the request came in on — so it points where the console can reach. Searches can take a while, as Prowlarr waits for every indexer; the console waits up to two minutes for an answer.
- Fetching that link for a **Usenet** indexer always answers `301` to the indexer's real link (`RedirectPermanent`): Prowlarr records the grab, and the NZB comes from the indexer. The link carries Prowlarr's key itself, so `FetchNzb` sends no `X-Api-Key` — a header would follow the redirect to the indexer. Errors come back as `429` (the indexer's grab limit) or `410` (the indexer disabled, or the release gone).
- A link to `localhost`, or with no host - from a Prowlarr set up behind a proxy - is pointed at `prowlarr-url`'s host (`ReachableNzbUrl`). A gzipped NZB is unpacked.

The indexer's HTTPS certificate has to chain to a compiled-in root. Checked in October 2026: NZBGeek, NZBFinder, DrunkenSlug, altHUB, NinjaCentral, nzb.su and NZBStars are behind Cloudflare (GTS Root R4), NZBPlanet uses Let's Encrypt (ISRG X1), usenet-crawler chains to USERTrust RSA.

Query-string keys - `apikey=`, `token=`, `key=`, `pass=` - are hidden wherever a URL is logged (`LogEscapePercent`), so neither Prowlarr's nor an indexer's key reaches `DebugInfo.txt`.

## The downloader

`NntpDownload` starts `usenet-connections` threads (8 by default, 16 at most), each with a connection of its own, spread over hardware threads 1, 4, 5 and 2 — away from the UI's thread 0, and from thread 3, the installer's, which only waits while this runs. TLS decryption is most of a connection's work on the 360, as it is for archive.org downloads.

Each thread takes the next article off a shared list — in file order, so the files fill roughly front to back — and:

1. sends `BODY <message-id>`;
2. on `222`, reads lines up to the lone `.`, undoing dot-stuffing (`..` at a line's start back to `.`);
3. decodes the yEnc, and writes the part at its offset in `F<nnn>`, under one write lock;
4. on `430`/`423` (no such article), counts it missing; on anything else — a dropped connection, a `4xx`/`5xx` — reconnects and tries again, up to three times.

A connection that can't reconnect after five tries puts its article back for another connection and stops. When every connection has stopped and articles are left, the provider was lost: the job fails with *Download failed*. A refused login (`481`/`482`) stops everything at once. A greeting other than `200`/`201` — usually `502`, over the plan's connection limit — costs only that connection; the others carry on.

An article that's missing - `430`, gone from the provider - or that fails its CRC is counted against its file (`NzbFile::missing`), with what the server answered logged. As NZBGet does, the download stops (*Too much is missing*) as soon as the bytes missing pass what the post's par2 volumes hold, since nothing could repair it then.

## par2

`Par2.cpp` implements Parchive 2.0: packets checked against their MD5 (`Md5Digest.cpp`), the main, file description and slice checksum packets of the index, and recovery slices - Reed-Solomon over GF(2^16) with the generator 0x1100B, each input slice's base 2^n for the n-th exponent coprime to 65535.

1. The index (`name.par2`) is always downloaded. Every downloaded file is matched to it by name, or - posted under a made-up name - by the MD5 of its first 16KB, and takes par2's name, so obfuscated posts install too.
2. Only the files that lacked articles are read, slice by slice, against the index's CRC32s; a file that never came is all damaged slices.
3. Volumes (`name.volNN+MM.par2`, MM recovery slices each) are fetched smallest first until they hold the damaged count plus two, and again if some arrive damaged.
4. The damaged slices are solved for: k equations from k recovery slices, the coefficient matrix inverted by Gauss-Jordan, then one pass over every intact slice and the recovery slices, each multiplied in and added to every slice being rebuilt (as many at a time as fit in 48MB). Each rebuilt slice is checked against its CRC before it's written.

`tools/test_usenet` checks this against par2cmdline's own output (`make_par2_data.py`): a RAR set with a hole, a cut-off volume, a volume that never came and one under a random name is repaired byte for byte.

Logins are sent only as `AUTHINFO USER`/`PASS`, and never logged.

### TLS

`usenet-ssl` is on by default, port 563. Connections use the same `XboxTLS` client and the same compiled-in roots as HTTPS (`addTrustAnchors`), so a provider whose certificate chains to a root not in `tools/trust-anchors/` fails with *Couldn't make a secure connection*. Google Trust Services, DigiCert G2, USERTrust RSA and Sectigo R46 were added for providers; ISRG (Let's Encrypt) was already there. With `usenet-ssl: off` the connection is plain TCP on port 119, which spares the CPU but sends the login in the clear.

## Finding the image

`FindImages` looks at each downloaded file by its name — from the NZB's subject, or the yEnc header when the subject had none:

- **RAR volumes** (`RarVolumeName`): `game.part01.rar` is set `game`, place 1; `game.rar` is place −1, then `game.r00` … `game.r99` are 0–99 and `game.s00` … 100 on. Each set's volumes are sorted by place and walked in order (`RarWalkVolumes`). Every file header for the same name continues the same file, and contributes a slice: its data, from the end of the header for `PACK_SIZE` bytes. A stored file's slices add up to its size, or a volume is missing.
- **A bare image** (`IsUsenetDiscImage`): `.iso`, or anything over a gigabyte that isn't an archive, par2 or video.

An entry in a RAR set is taken if it looks like a disc image, and is stored (`-m0`), not encrypted, and complete. A compressed one is refused with a message that says so — decompressing RAR on the console isn't implemented. Images are installed in name order, so a two-disc post installs Disc 1 then Disc 2.

`SpanSource` serves `GodConvert` reads from the slices: a binary search for the slice an offset falls in, then reads across into the next as needed, with one file open at a time. The converter reads the disc's file system out of order first and then its data mostly in order, through the usual `ReadAheadSource`, so this costs about what reading a single file would.

## Names

What the dashboard shows is the Store's name for the disc's title ID when its lists have one (with *(Disc N)* for a multi-disc game), and otherwise the release title tidied by `UsenetDisplayName`: dots and underscores to spaces, the `-GROUP` tag off the end.

## Room

The check before downloading wants twice the download's size — the files, and the game converted from them — plus a margin. `InstallImage` checks again, exactly, once it knows how big the game is. Reading the image straight out of the volumes is what keeps this at twice rather than three times: nothing is ever unpacked.

## Not done yet

- Decompressing RAR (and 7z).
- Resuming a download across a restart: the staging folder is cleared when Omni360 starts.

