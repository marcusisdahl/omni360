"""
Makes the data test_usenet.cpp checks Omni360's Usenet pieces against:

    python make_test_data.py [folder]      (default: .\\data)

  image.iso            a 10MB "disc image" of seeded random bytes
  old\\game.rar, game.r00 ...         it as a stored (-m0) RAR4 set, 3MB
                                       volumes, old-style names
  new\\game.part1.rar ... part10.rar   the same, 1MB volumes, new-style names -
                                       part10 has to come after part9
  articles\\NNN.txt     game.part1.rar posted as yEnc articles of 100,000
                       bytes - each as NNTP hands a body over once its
                       dot-stuffing is undone
  single.txt           a small file posted as one article, no =ypart line
  damaged.txt          an article with a byte changed after its CRC was taken
  post.nzb             the articles' NZB: segments out of order, one listed
                       twice, a subject with &quot; entities, and a .par2

There's no RAR tool here to make the archives with, so they're written by
hand to the RAR 2.9/4.x layout (RarHeaders.cpp documents it): a marker block,
a main header, one file header per volume - split before and after as it
continues - with the stored data after it, and an end-of-archive header.
"""
import os, random, struct, sys, zlib

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
IMAGE_SIZE = 10 * 1024 * 1024
ARTICLE_SIZE = 100000


def header(body):
    """A RAR4 header: HEAD_CRC, the low 16 bits of the CRC of the rest."""
    return struct.pack("<H", zlib.crc32(body) & 0xFFFF) + body


def rar4_volumes(name, data, volume_data):
    marker = b"Rar!\x1a\x07\x00"
    volumes = []
    crc = zlib.crc32(data) & 0xFFFFFFFF
    count = (len(data) + volume_data - 1) // volume_data
    for v in range(count):
        part = data[v * volume_data:(v + 1) * volume_data]
        main_flags = 0x0001 | (0x0010 if "part" in name else 0) | (0x0100 if v == 0 else 0)
        main = header(struct.pack("<BHH", 0x73, main_flags, 13) + b"\x00" * 6)
        flags = 0x8000 | (0x0001 if v > 0 else 0) | (0x0002 if v < count - 1 else 0)
        encoded_name = name.encode()
        fixed = struct.pack("<IIBIIBBHI", len(part), len(data), 2, crc, 0, 29, 0x30, len(encoded_name), 0x20)
        size = 7 + len(fixed) + len(encoded_name)
        file_header = header(struct.pack("<BHH", 0x74, flags, size) + fixed + encoded_name)
        end = header(struct.pack("<BHH", 0x7B, 0x0001 if v < count - 1 else 0, 7))
        volumes.append(marker + main + file_header + part + end)
    return volumes


def yenc(data, line=128):
    out = bytearray()
    col = 0
    for b in data:
        c = (b + 42) & 0xFF
        if c in (0, 10, 13, 61) or (col == 0 and c in (9, 32, 46)) or (col == line - 1 and c in (9, 32)):
            out += b"="
            c = (c + 64) & 0xFF
            col += 1
        out.append(c)
        col += 1
        if col >= line:
            out += b"\r\n"
            col = 0
    if col:
        out += b"\r\n"
    return bytes(out)


def article(name, data, part, total, begin, size, damage=False):
    body = yenc(data)
    if damage:
        body = bytearray(body)
        body[len(body) // 2] = ord("A") if body[len(body) // 2] != ord("A") else ord("B")
        body = bytes(body)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    return (b"=ybegin part=%d total=%d line=128 size=%d name=%s\r\n" % (part, total, size, name.encode()) +
            b"=ypart begin=%d end=%d\r\n" % (begin + 1, begin + len(data)) + body +
            b"=yend size=%d part=%d pcrc32=%08x\r\n" % (len(data), part, crc))


def main():
    rng = random.Random(360)
    image = bytes(rng.getrandbits(8) for _ in range(IMAGE_SIZE))
    for sub in ("old", "new", "articles"):
        os.makedirs(os.path.join(OUT, sub), exist_ok=True)
    open(os.path.join(OUT, "image.iso"), "wb").write(image)

    old = rar4_volumes("Game.iso", image, 3 * 1024 * 1024)
    for i, v in enumerate(old):
        open(os.path.join(OUT, "old", "game.rar" if i == 0 else "game.r%02d" % (i - 1)), "wb").write(v)
    new = rar4_volumes("Game\\Game.iso", image, 1024 * 1024)
    for i, v in enumerate(new):
        open(os.path.join(OUT, "new", "game.part%d.rar" % (i + 1)), "wb").write(v)

    # The first new-style volume, posted.
    posted = new[0]
    parts = [posted[i:i + ARTICLE_SIZE] for i in range(0, len(posted), ARTICLE_SIZE)]
    for n, data in enumerate(parts):
        open(os.path.join(OUT, "articles", "%03d.txt" % (n + 1)), "wb").write(
            article("game.part1.rar", data, n + 1, len(parts), n * ARTICLE_SIZE, len(posted)))

    small = b"=.\r\n\x00 hello \x00\xff" * 50
    crc = zlib.crc32(small) & 0xFFFFFFFF
    open(os.path.join(OUT, "single.txt"), "wb").write(
        b"=ybegin line=128 size=%d name=notes with spaces.nfo\r\n" % len(small) + yenc(small) +
        b"=yend size=%d crc32=%08x\r\n" % (len(small), crc))
    open(os.path.join(OUT, "damaged.txt"), "wb").write(
        article("game.part1.rar", parts[0], 1, len(parts), 0, len(posted), damage=True))

    # The NZB: segments shuffled, number 2 twice.
    order = list(range(1, len(parts) + 1))
    rng.shuffle(order)
    order.insert(3, 2)
    segments = "".join('      <segment bytes="%d" number="%d">part%dof%d.x7q@example.invalid</segment>\n'
                       % (ARTICLE_SIZE + 2000, n, n, len(parts)) for n in order)
    nzb = ('<?xml version="1.0" encoding="iso-8859-1" ?>\n'
           '<!DOCTYPE nzb PUBLIC "-//newzBin//DTD NZB 1.1//EN" "http://www.newzbin.com/DTD/nzb/nzb-1.1.dtd">\n'
           '<nzb xmlns="http://www.newzbin.com/DTD/2003/nzb">\n'
           '  <head><meta type="title">Game</meta></head>\n'
           '  <file poster="someone &lt;x@y&gt;" date="1760000000" subject="Game [1/3] - &quot;game.part1.rar&quot; yEnc (1/%d)">\n'
           '    <groups><group>alt.binaries.test</group></groups>\n'
           '    <segments>\n%s    </segments>\n'
           '  </file>\n'
           '  <file poster="someone" date="1760000000" subject="Game [2/3] - &quot;game.vol00+01.par2&quot; yEnc (1/1)">\n'
           '    <groups><group>alt.binaries.test</group></groups>\n'
           '    <segments>\n      <segment bytes="5000" number="1">&lt;par.1@example.invalid&gt;</segment>\n    </segments>\n'
           '  </file>\n'
           '  <file poster="someone" date="1760000000" subject="no quoted name here yEnc (1/1)">\n'
           '    <groups><group>alt.binaries.test</group></groups>\n'
           '    <segments>\n      <segment bytes="900" number="1">plain.1@example.invalid</segment>\n    </segments>\n'
           '  </file>\n'
           '</nzb>\n') % (len(parts), segments)
    open(os.path.join(OUT, "post.nzb"), "w", newline="\n").write(nzb)

    print("wrote", OUT, "-", len(old), "old-style volumes,", len(new), "new-style,", len(parts), "articles")


if __name__ == "__main__":
    main()
