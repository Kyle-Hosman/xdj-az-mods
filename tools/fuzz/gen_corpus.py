#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Writes the seed corpora under tools/fuzz/corpus/<name>/ (deterministic: the same bytes on
every run). Seeds are small and synthetic: real-shaped files, no copyrighted content.
Crash regressions (regress-*) are added by hand next to them and are never touched here.
Usage: tools/fuzz/gen_corpus.py [name...]"""
import os, struct, sys, zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "corpus")
SEEDS = {}


def seed(name):
    def deco(fn):
        SEEDS[name] = fn
        return fn
    return deco


@seed("azconf")
def azconf():
    conf = (b"# azmod.conf: AZ-MODS settings\nversion = 1\ntheme = dark\npadcolor.on = on\n"
            b"padcolor.0 = 3\npadcolor2.1 = native\npinned = slot.3,drumroll.bank\n"
            b"drumroll.bank = 2\nvinyl.brake = 0.50\nmymod:speed = 12\n")
    yield "load-basic", b"\0" + conf
    yield "load-crlf-bom", b"\0" + b"\xef\xbb\xbf" + conf.replace(b"\n", b"\r\n")
    yield "load-dups-junk", b"\0" + conf + b"theme = light\nnot a line\n\x01bad\n  # indented\n\ntheme=day"
    yield "load-newer", b"\0# x\nversion = 2\na = b\n"
    yield "load-utf8", b"\0version = 1\nname = Caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xb5\nbad = \xc0\xaf\n"
    cfg = b"1011000111010111100101101\n3 4 -1 27 1 0 1 2 3\n5\n"
    yield "migrate-full", b"\1\x07" + cfg + b"\0dark\n\0" + b"0.75\n\0" + b"3"
    yield "migrate-short", b"\1\x00" + b"1111111111111\n1 2 3\n"
    yield "migrate-corrupt", b"\1\x02" + b"10102\n9 9 9 9\n18446744073709551615\n\0 light \0 1.5\0 7"


@seed("bus")
def bus():
    def crc16_x25(b):
        c = 0xFFFF
        for x in b:
            c ^= x
            for _ in range(8):
                c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
        return ~c & 0xFFFF
    def stamp(f, n):
        c = crc16_x25(f[:n])
        return f[:n] + bytes([c & 0xFF, c >> 8]) + f[n + 2:]
    cfg = bytes(8)
    mixer = bytearray(128)
    mixer[0x16] = 0x40; mixer[34] = 0x10; mixer[0x2e:0x4b] = bytes(range(29))
    yield "mixer-crc", b"\x00\x80" + cfg + stamp(bytes(mixer), 96)
    yield "mixer-short-read", b"\x0c\x20" + cfg + stamp(bytes(mixer), 96)
    erp = bytearray(30); erp[10] = 0x01; erp[24] = 0x03
    yield "erp-crc", b"\x01\x40" + cfg + stamp(bytes(erp), 28)
    yield "erp-menu", b"\x05\x1e" + b"\xff" * 8 + stamp(bytes(erp), 28)
    yield "deck", b"\x02\x40" + cfg + bytes(range(40))
    yield "other", b"\x03\x10" + cfg + b"\x41" * 20


@seed("cmjson")
def cmjson():
    js = (b'{"id": "beat-meter", "name": "Beat Meter", "version": "1.0.2", "author": "someone",\n'
          b' "summary": "Shows the beat \\"phase\\"", "api": 1, "hooks": false, "min_core": "1.2.0",\n'
          b' "source": "https://example.invalid/beat-meter", "firmware": ["ep147-0123456789abcdef", "any"],\n'
          b' "tags": [], "size": 12345, "extra": null}')
    yield "json-full", b"\0" + js
    yield "json-empty", b"\0{}"
    yield "json-dupkey", b'\0{"id":"a","id":"b"}'
    yield "json-nested", b'\0{"id":"abc","x":{"y":1}}'
    yield "json-unterminated", b'\0{"id":"abc'
    yield "json-longname", b'\0{"id":"' + b"a" * 60 + b'","name":"' + b"N" * 100 + b'"}'
    yield "json-fw-escaped", b'\0{"firmware":["a\\"b", "any"]}'
    sha = b"0123456789abcdef" * 4
    yield "approved", b"\1" + b"beat-meter " + sha + b"\nother-mod " + sha + b"\nbeat-meter " + sha.upper() + b"\nx y z\n"
    yield "approved-many", b"\1" + b"".join(b"mod-%03d " % i + sha + b"\n" for i in range(70))
    yield "versions", b"\x021.2.3\x001.10.0"
    yield "versions-bad", b"\x029999.0.1\x0099999.0.0"


def png_chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)


def png_file(w, h, ct=6, depth=8, extra=b"", plte=None):
    bpp = {6: 4, 2: 3, 3: 1}[ct]
    raw = b"".join(b"\0" + bytes((x * 7 + y) & 0xFF for x in range(w * bpp)) for y in range(h))
    out = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, 0))
    if plte:
        out += png_chunk(b"PLTE", plte)
    return out + extra + png_chunk(b"IDAT", zlib.compress(raw)) + png_chunk(b"IEND", b"")


@seed("png")
def png():
    yield "rgba16", b"\0" + png_file(16, 16)
    yield "rgb8", b"\1" + png_file(8, 8, ct=2)
    yield "pal4", b"\0" + png_file(4, 4, ct=3, plte=b"\x10\x20\x30" * 4, extra=png_chunk(b"tRNS", b"\xff\x80"))
    meta = png_chunk(b"tEXt", b"Software\0x") + png_chunk(b"gAMA", b"\0\0\xb1\x8f") + png_chunk(b"sRGB", b"\0")
    yield "meta", b"\1" + png_file(8, 8, extra=meta)
    yield "split-idat", b"\1" + png_file(4, 4)[:-12] + png_chunk(b"IDAT", b"") + png_chunk(b"IEND", b"")


def pdb_file(pl, tracks, names=()):
    """A minimal export.pdb in rekordbox's layout (pdb_sync.c's header comment): every table
    0..13 has an index page; the ones pdb_sync reads also get one data page."""
    data_types = (0, 1, 2, 3, 4, 5, 7, 8, 13)
    T = 14
    pages = {}
    tables = []
    nxt = 1
    for t in range(T):
        idx = nxt; nxt += 1
        if t in data_types:
            dp = nxt; nxt += 1
            tables.append([t, None, idx, dp])
        else:
            tables.append([t, None, idx, idx])
    ec = nxt
    for tb in tables:
        tb[1] = ec; ec += 1
    nu = ec
    def page(p, typ, nx, flags, rows):
        b = bytearray(pl)
        struct.pack_into("<IIIII", b, 4, p, typ, nx, 0, 0)
        heap, ofs = bytearray(), []
        for r in rows:
            ofs.append(len(heap))
            heap += r + bytes((-len(r)) % 4)
        b[0x28:0x28 + len(heap)] = heap
        n = len(rows)
        b[0x18] = n & 0xff
        b[0x19] = ((n & 7) << 5) | (n >> 8 & 0x1f)
        b[0x1a] = n >> 3
        b[0x1b] = flags
        struct.pack_into("<HHHH", b, 0x1c, pl - 0x28 - len(heap) - 0x24 * ((n + 15) // 16), len(heap), 0x1fff, 0x1fff)
        for g in range((n + 15) // 16):
            base = pl - g * 0x24
            k = min(16, n - 16 * g)
            struct.pack_into("<H", b, base - 4, (1 << k) - 1)
            struct.pack_into("<H", b, base - 2, 0)
            for i in range(k):
                struct.pack_into("<H", b, base - 6 - 2 * i, ofs[16 * g + i])
        if flags & 0x40:
            struct.pack_into("<H", b, 0x24, 8)
        return b
    def dstr(s):
        return bytes([((len(s) + 1) << 1) | 1]) + s
    def track_row(tid, path):
        r = bytearray(0x88)
        struct.pack_into("<I", r, 0x48, tid)
        strs = bytearray()
        offs = []
        for k in range(21):
            offs.append(0x88 + len(strs))
            strs += dstr(path if k == 20 else (b"Title %d" % tid if k == 17 else b""))
        for k, o in enumerate(offs):
            struct.pack_into("<H", r, 0x5e + 2 * k, o)
        return bytes(r + strs)
    def named_row(t, i, nm):
        if t in (1, 4, 13):
            return struct.pack("<I", i) + dstr(nm)
        if t == 5:
            return struct.pack("<II", i, i) + dstr(nm)
        if t == 2:
            return struct.pack("<HHI", 0x60, 0, i) + b"\x03" + bytes([10]) + dstr(nm)
        return struct.pack("<HHIII", 0x80, 0, 0, i - 1, i) + bytes(5) + bytes([0x16]) + dstr(nm)
    for t, ecx, idx, dp in tables:
        pages[idx] = page(idx, t, dp if dp != idx else ecx, 0x64, [])
        if dp != idx:
            rows = []
            if t == 0:
                rows = [track_row(i + 1, p) for i, p in enumerate(tracks)]
            elif t in (1, 2, 3, 4, 5, 13):
                rows = [named_row(t, i + 1, nm) for i, nm in enumerate(names)]
            elif t == 7:
                rows = [struct.pack("<IIIII", 0, 0, 0, 9, 0) + dstr(b"Old")]
            elif t == 8:
                rows = [struct.pack("<III", 1, 1, 9)] if tracks else []
            pages[dp] = page(dp, t, ecx, 0x24, rows)
    hdr = bytearray(pl)
    struct.pack_into("<IIIIIII", hdr, 0, 0, pl, T, nu, 0, 1, 0)
    for i, (t, ecx, idx, dp) in enumerate(tables):
        struct.pack_into("<IIII", hdr, 0x1c + 16 * i, t, ecx, idx, dp)
    out = bytearray(hdr)
    for p in range(1, nxt):
        out += pages[p]
    return bytes(out)


@seed("pdbsync")
def pdbsync():
    for pl, sel in ((512, 0),):
        dst = pdb_file(pl, [b"/Contents/b.mp3"], [b"House"])
        src = pdb_file(pl, [b"/Contents/a.mp3", b"/Contents/b.mp3"], [b"House", b"Techno"])
        dst, src = dst.rstrip(b"\0"), src.rstrip(b"\0")  # the harness pads with zeros again
        yield "sync-%d" % pl, bytes([sel]) + struct.pack("<H", len(dst)) + dst + src
    dst = pdb_file(512, [b"/Contents/b.mp3"]).rstrip(b"\0")
    yield "nosource", b"\x04" + struct.pack("<H", len(dst)) + dst


def elf_so(exports=(b"az_mod_v1_entry", b"_init"), needed=(b"libc.so.6",), bad_strtab=False):
    """A minimal aarch64 ET_DYN: section headers for .dynsym, .dynstr and .dynamic only (all
    cm_elf_check reads)."""
    dynstr = b"\0"
    names = []
    for e in exports + needed:
        names.append(len(dynstr)); dynstr += e + b"\0"
    if bad_strtab:
        dynstr = dynstr[:-1]
    syms = bytes(24)
    for i in range(len(exports)):
        syms += struct.pack("<IBBHQQ", names[i], 0x12, 0, 7, 0x1000, 8)
    dyn = b""
    for i in range(len(needed)):
        dyn += struct.pack("<qQ", 1, names[len(exports) + i])
    dyn += struct.pack("<qQ", 0, 0)
    body = bytearray(64)
    off_sym = len(body); body += syms
    off_str = len(body); body += dynstr
    body += bytes((-len(body)) % 8)
    off_dyn = len(body); body += dyn
    body += bytes((-len(body)) % 8)
    shoff = len(body)
    sh = bytes(64)
    sh += struct.pack("<IIQQQQIIQQ", 0, 11, 2, 0, off_sym, len(syms), 2, 1, 8, 24)
    sh += struct.pack("<IIQQQQIIQQ", 0, 3, 2, 0, off_str, len(dynstr), 0, 0, 1, 0)
    sh += struct.pack("<IIQQQQIIQQ", 0, 6, 3, 0, off_dyn, len(dyn), 2, 0, 8, 16)
    body += sh
    eh = b"\x7fELF\x02\x01\x01" + bytes(9) + struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 0, shoff, 0, 64, 56, 0, 64, 4, 0)
    body[0:64] = eh
    return bytes(body)


@seed("elfcheck")
def elfcheck():
    yield "ok", elf_so()
    yield "forbidden-export", elf_so(exports=(b"az_mod_v1_entry", b"open"))
    yield "bad-lib", elf_so(needed=(b"libc.so.6", b"libX11.so.6"))
    yield "no-entry", elf_so(exports=(b"_fini",))
    yield "unterminated-strtab", elf_so(needed=(b"libm.so.6",), bad_strtab=True)


@seed("stemname")
def stemname():
    yield "rank-exact", b"\0Track One\0track one"
    yield "rank-underscore", b"\0Track One_\0Track One"
    yield "rank-hex", b"\0Track One-0a1b2c3d\0Track One"
    yield "rank-other", b"\0Track One (2)\0Track One"
    yield "slot-vocals", b"\1Track One - Vocals.ogg"
    yield "slot-drums", b"\1drums.ogg"
    yield "json-same", b'\2{"file": "Music/Track One.mp3", "size": 1000, "fp": "fnv1a64:0123456789abcdef", "v": 1}'
    yield "json-escapes", b'\2{"file": "Music/Tr\\u00e9ck \\ud83c\\udfb5 \\"x\\" \\/ One.mp3", "size": 1000}'
    yield "json-other", b'\2{"file": "Other/x.mp3", "size": 5}'
    head = b'{"size": 1000, "file": "'
    body = head + b"\\u0041" * 678  # 6 bytes in, 1 out: reaches the 4095-byte read limit under 1 KB out
    yield "json-u-at-limit", b"\2" + body + b"a" * (4093 - len(body)) + b"\\u0041\""


def wav(rate, chans, bits, frames, tag=1, extensible=False, extra=b""):
    """A tiny synthetic WAV: a short decaying click, no recorded audio."""
    import math
    bpf = bits // 8 * chans
    data = bytearray()
    for i in range(frames):
        v = math.sin(i * 0.7) * math.exp(-i / 6.0)
        for _ in range(chans):
            if tag == 3:
                data += struct.pack("<f" if bits == 32 else "<d", v)
            elif bits == 8:
                data += bytes([int(128 + 127 * v) & 0xFF])
            else:
                data += int(v * (2 ** (bits - 1) - 1)).to_bytes(bits // 8, "little", signed=True)
    if extensible:
        guid = struct.pack("<H", tag) + bytes([0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71])
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, chans, rate, rate * bpf, bpf, bits, 22, bits, 3) + guid
    else:
        fmt = struct.pack("<HHIIHH", tag, chans, rate, rate * bpf, bpf, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + extra + b"data" + struct.pack("<I", len(data)) + bytes(data)
    return b"RIFF" + struct.pack("<I", len(body)) + body


@seed("drumwav")
def drumwav():
    yield "pcm16-stereo", b"\0" + wav(44100, 2, 16, 64)
    yield "pcm8-mono-22k", b"\0" + wav(22050, 1, 8, 48)
    yield "pcm24-48k", b"\0" + wav(48000, 2, 24, 40)
    yield "float32-ext", b"\0" + wav(96000, 2, 32, 40, tag=3, extensible=True)
    yield "float64", b"\0" + wav(44100, 1, 64, 20, tag=3)
    yield "list-chunk", b"\0" + wav(44100, 2, 16, 16, extra=b"LIST" + struct.pack("<I", 5) + b"INFOx\0")
    yield "alaw", b"\0" + wav(8000, 1, 8, 16, tag=6)
    yield "name-kick", b"\1" + b"01 Kick-Hard.WAV"
    yield "name-hat", b"\1" + b"hi_hat open.ogg"


@seed("cmcat")
def cmcat():
    cat = (b"v1\n"
           b"repo|https://example.invalid/r0/azmods-repo.json|Community|1|1|\n"
           b"repo|https://example.invalid/r1/azmods-repo.json|Added|0|0|could not fetch\n"
           b"mod|https://example.invalid/r0/azmods-repo.json|beat-meter|Beat Meter|1.0.2|someone|Shows the beat|tools|1|any|12000|icon.png||1.2.0|A longer text.|64|||0|2019-10-08\n"
           b"mod|https://example.invalid/r1/azmods-repo.json|other-mod|Other|0.1.0|x|y|fun|0|ep147-0123456789abcdef,ep147-1|5|||||0|https://example.invalid/r9/azmods-repo.json|other|2|20191008\n"
           b"mod|https://example.invalid/r0/azmods-repo.json|echo-out|Echo Out|1.0.0|dubwise|s|audio|0|any|1000|||||0||||2019-09-14|1840\n"
           b"mod|https://example.invalid/r0/azmods-repo.json|big-clock|Big Clock|1.0.0|nightowl|s|audio|0|any|1000||0.9.0|||0||||2019-10-12|0012|extra\n"
           b"mod|https://example.invalid/r0/azmods-repo.json|loop-kit|Loop Kit|1.0.0|carol|s|visual|0|any|1000|||||0||||2019-10-01|99999999999999\n"
           b"more|3\n")
    status = b"state=running\nop=install\nid=beat-meter\nmsg=Downloading\ndone=10\ntotal=100\n"
    yield "home", b"\x00\x00" + cat + b"\0" + status
    yield "browse", b"\x01\x00" + cat
    yield "repos", b"\x02\x00" + cat + b"\0state=done\nop=remove\nid=other-mod\n"
    yield "modpage", b"\x03\x01" + cat + b"\0state=done\nop=enable\nid=beat-meter\n"
    yield "full-modpage", b"\x0b\x00" + cat
    yield "badversion", b"\x01\x00v2\nmod|a|b\n"
    yield "browse-sorted", b"\x59\x03" + cat
    yield "add-repository-keyboard", b"\x0f\x00" + cat
    yield "home-shelves", b"\x09\x00" + cat
    yield "home-knob", b"\x89\x04" + cat
    yield "all-downloads", b"\x39\x01" + cat


@seed("sdklib")
def sdklib():
    def row(texts, nulls=()):
        out = b"\x01"
        for c in range(8):
            t = texts[c] if c < len(texts) else b""
            if c in nulls:
                out += b"\x00" + bytes(3)
            elif len(t) < 32:
                out += b"\x01" + bytes([len(t)]) + t + bytes(3)
            else:
                out += b"\x02" + bytes([len(t) // 4, len(t) % 4]) + t + bytes(3)
        return out
    yield "playlists", b"\x00\x00\x01" + row([b"\x01", b"\x00", b"Warmup", b"\x00"]) + row([b"", b"", b"Folder"], nulls=(2,)) + b"\x00"
    yield "tracks-all", b"\x01\x00\x01" + row([b"", b"Title", b"Artist", b"Album", b"8A", b"/Contents/a.mp3"]) + b"\x00"
    yield "tracks-long", b"\x01\x01\x01" + row([b"", b"T" * 300, b"A" * 900, b"", b"K" * 40, b"/C/" + b"p" * 250], nulls=(3,)) + b"\x00"
    yield "tracks-cap", b"\x01\xff\x01" + row([b"", b"x"]) + row([b"", b"y"]) + row([b"", b"z"]) + b"\x00"
    yield "tracks-error", b"\x01\x00\x00" + row([b"", b"x"]) + b"\x00"
    yield "sysfs", b"\x02/sys/devices/platform/usb/1-1/1-1:1.0/host0/target0:0:0/0:0:0:0/block/sda"
    yield "mounts", b"\x03/dev/sda1 /media/usb1 vfat rw 0 0\n/dev/sdb1 /media/a\\040b vfat rw 0 0\nproc /proc proc rw 0 0\n"
    yield "cursrc", b"\x04/media/usb1/Contents/a.mp3\n"


def tzif(times, idx, offs, footer=b"\n", ver=b"2"):
    """A TZif file: a minimal version 1 part, then the 64-bit part and the footer."""
    def hdr(nt, nty):
        return b"TZif" + ver + b"\0" * 15 + struct.pack(">6I", 0, 0, 0, nt, nty, 4)
    v1 = hdr(0, 1) + struct.pack(">iBB", 0, 0, 0) + b"UTC\0"
    v2 = hdr(len(times), len(offs)) + b"".join(struct.pack(">q", t) for t in times) + bytes(idx)
    v2 += b"".join(struct.pack(">iBB", o, int(k == 1), 0)
                   for k, o in enumerate(offs)) + b"MST\0"
    return v1 + v2 + footer


@seed("clocktz")
def clocktz():
    us = [1173603600 + k * 31556952 for k in range(20)]
    yield "us-fat", b"\0" + tzif(us, [1 - (k & 1) for k in range(20)], [-7 * 3600, -6 * 3600],
                                  b"\nMST7MDT,M3.2.0,M11.1.0\n")
    yield "us-slim", b"\1" + tzif(us[:2], [1, 0], [-7 * 3600, -6 * 3600], b"\nMST7MDT,M3.2.0,M11.1.0\n")
    yield "fixed", b"\2" + tzif([-(1 << 59)], [0], [19800], b"\nIST-5:30\n")
    yield "quoted", b"\3" + tzif([0], [0], [-3 * 3600], b"\n<-03>3\n")
    yield "no-footer", b"\4" + tzif(us[:3], [1, 0, 1], [-7 * 3600, -6 * 3600], b"")
    yield "v1-only", b"\5" + tzif([], [], [0], b"", ver=b"\0")

@seed("wzsave")
def wzsave():
    for t in ("2/1", "1/1", "1/2", "1/4", "1/8", "1/16"):
        yield "level-" + t.replace("/", "-"), t.encode()
    yield "bad-1-32", b"1/32"
    yield "bad-zero", b"01/4"
    yield "bad-tail", b"1/4 "
    yield "bad-empty", b""
    yield "live-stock", b"\x01\x00\x04\x00"

@seed("colwidths")
def colwidths():
    yield "two", b"5:300 6:120\n"
    yield "all", b"2:52 3:78 5:345 6:110 7:90 8:60\n"
    yield "squeeze", b"5:900 6:900 7:900\n"
    yield "empty", b""
    yield "bad-dup", b"6:100 6:120\n"
    yield "bad-range", b"6:20\n"

@seed("colfields")
def colfields():
    yield "two", b"3 GENRE\n7 COMMENTS\n"
    yield "all", (b"1 RATING\n2 COLOR\n3 TIME\n7 BITRATE\n8 DJ PLAY COUNT\n9 ORIGINAL ARTIST\n"
                  b"10 DATE ADDED\n")
    yield "crlf", b"\r\n3 LABEL\r\n8 REMIXER"
    yield "empty", b""
    yield "bad-dup", b"3 GENRE\n3 ALBUM\n"
    yield "bad-track", b"3 TRACK\n"
    yield "bad-range", b"11 GENRE\n"
    yield "text-utf8", b"Caf\xc3\xa9 \xe2\x98\x85 \xf0\x9f\x8e\xb5 \xc0\xaf\xed\xa0\x80\x01"

@seed("ofscache")
def ofscache():
    yield "taken", b"AZOFS1 1000 2000 3000 1042\n"
    yield "negative", b"AZOFS1 1000 2000 3000 -8192\n"
    yield "other-size", b"AZOFS1 1001 2000 3000 1042\n"
    yield "past-window", b"AZOFS1 1000 2000 3000 8193\n"
    yield "no-newline", b"AZOFS1 1000 2000 3000 1042"
    yield "huge", b"AZOFS1 99999999999999999999 2000 3000 1042\n"

@seed("inputmem")
def inputmem():
    def ent(name, v):
        return name + b"\0\x01\x05\x01" + struct.pack("<I", v)
    def strent(name, v):
        return name + b"\0\x01" + bytes([len(v) + 2]) + b"\x05" + v + b"\0"
    base = (b"Setting\0" + strent(b"HistoryName", b"HISTORY") + ent(b"InputSelect0", 0)
            + ent(b"InputSelect1", 0) + ent(b"InputSelect2", 2) + ent(b"InputSelect3", 3)
            + ent(b"InputSelectMemory", 1) + ent(b"JogLcdBrightness", 2))
    yield "deck-shape", base
    yield "memory-off", base.replace(ent(b"InputSelectMemory", 1), ent(b"InputSelectMemory", 0))
    yield "truncated", base[:-5]
    yield "duplicate", base + ent(b"InputSelect2", 1)

@seed("fcopy")
def fcopy():
    # "name\x01type\x02size\n" per entry under the source folder "S"; type 1 file, 2 folder
    def ent(name, t, size=0):
        return name.encode() + b"\x01" + bytes([t]) + b"\x02" + str(size).encode() + b"\n"
    yield "plain", ent("a.mp3", 1, 100) + ent("Inner", 2) + ent("Inner/b.mp3", 1, 5)
    yield "junk", ent("._a.mp3", 1, 4) + ent(".DS_Store", 1, 6) + ent("c.azcp", 1, 1)
    yield "dotdot", ent("..", 1, 1) + ent("x/y", 1, 1)


def box(t, payload):
    return struct.pack(">I", 8 + len(payload)) + t + payload


def m4a_file(nsamp=4, co64=False, ver=0, elst_v=0):
    asc = bytes([0x12, 0x10])  # AAC-LC, 44.1 kHz, stereo
    esds = (b"\0\0\0\0" + b"\x03\x19" + b"\0\x01\0" + b"\x04\x11" + bytes([0x40, 0x15]) + bytes(11)
            + b"\x05\x02" + asc + b"\x06\x01\x02")
    mp4a = bytes(6) + b"\0\x01" + struct.pack(">H", ver) + bytes(6) + struct.pack(">HHHHI", 2, 16, 0, 0, 44100 << 16)
    mp4a += bytes(16 if ver == 1 else 36 if ver == 2 else 0) + box(b"esds", esds)
    stsd = b"\0\0\0\0" + struct.pack(">I", 1) + box(b"mp4a", mp4a)
    stsz = b"\0\0\0\0" + struct.pack(">II", 0, nsamp) + b"".join(struct.pack(">I", 8) for _ in range(nsamp))
    stsc = b"\0\0\0\0" + struct.pack(">I", 1) + struct.pack(">III", 1, nsamp, 1)
    data_at = 0  # patched below
    if co64:
        stco = b"\0\0\0\0" + struct.pack(">I", 1) + struct.pack(">Q", 0)
    else:
        stco = b"\0\0\0\0" + struct.pack(">I", 1) + struct.pack(">I", 0)
    stbl = box(b"stsd", stsd) + box(b"stsz", stsz) + box(b"stsc", stsc) + box(b"co64" if co64 else b"stco", stco)
    hdlr = bytes(8) + b"soun" + bytes(12) + b"\0"
    minf = box(b"stbl", stbl)
    mdia = box(b"hdlr", hdlr) + box(b"minf", minf)
    if elst_v:
        elst = bytes([1, 0, 0, 0]) + struct.pack(">IQQ", 1, 1000, 2112) + b"\0\x01\0\0"
    else:
        elst = bytes(4) + struct.pack(">III", 1, 1000, 2112) + b"\0\x01\0\0"
    trak = box(b"edts", box(b"elst", elst)) + box(b"mdia", mdia)
    tool = box(b"\xa9too", box(b"data", bytes(8) + b"Lavf58.76.100"))
    moov = box(b"moov", box(b"trak", trak) + box(b"udta", box(b"meta", bytes(4) + box(b"ilst", tool))))
    ftyp = box(b"ftyp", b"M4A \0\0\0\0M4A mp42isom")
    head = ftyp + moov
    mdat = box(b"mdat", bytes(range(8)) * nsamp)
    off = len(head) + 8
    out = bytearray(head + mdat)
    i = out.find(b"co64" if co64 else b"stco")
    if co64:
        struct.pack_into(">Q", out, i + 4 + 8, off)
    else:
        struct.pack_into(">I", out, i + 4 + 8, off)
    return bytes(out)


def mp3_file(vendor=b"LAME3.100", tag=b"Info", id3=True):
    hdr = bytes([0xFF, 0xFB, 0x90, 0x64])  # MPEG-1 Layer III, 128 kbps, 44.1 kHz, joint stereo
    frame = hdr + bytes(32) + tag + bytes(116) + vendor + bytes(417 - 4 - 32 - 4 - 116 - len(vendor))
    pre = b"ID3\x04\0\0\0\0\0\x0a" + bytes(10) if id3 else b""
    return pre + frame + hdr + bytes(413)


@seed("mp4")
def mp4():
    yield "m4a", b"\0" + m4a_file()
    yield "m4a-co64-v1", b"\0" + m4a_file(nsamp=3, co64=True, ver=1, elst_v=1)
    yield "m4a-v2", b"\0" + m4a_file(nsamp=2, ver=2)
    yield "mp3-lame", b"\1" + mp3_file()
    yield "mp3-xing", b"\1" + mp3_file(vendor=b"Lavc58.91", tag=b"Xing", id3=False)


def anlz_file(sections):
    """A synthetic analysis file: the PMAI header and the given (tag, header, body) sections."""
    out = b""
    for tag, hdr, body in sections:
        out += tag + struct.pack(">II", 12 + len(hdr), 12 + len(hdr) + len(body)) + hdr + body
    return b"PMAI" + struct.pack(">II", 28, 28 + len(out)) + b"\0" * 16 + out


def pcpt(slot, ms):
    e = b"PCPT" + struct.pack(">IIIIIHH", 0x1c, 0x38, slot, 0, 0x10000, 0xffff, 0xffff)
    e += bytes([1, 0, 0, 0]) + struct.pack(">II", ms, 0xffffffff)
    return e + b"\0" * (0x38 - len(e))


def pcp2(slot, ms, rgb, comment=b""):
    e = b"PCP2" + struct.pack(">III", 0x10, 0, slot) + bytes([1, 0, 0, 0])
    e += struct.pack(">II", ms, 0xffffffff) + b"\0" * 12 + struct.pack(">I", len(comment)) + comment
    e += bytes([0x26, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255]) + b"\0" * 16
    return e[:8] + struct.pack(">I", len(e)) + e[12:]


@seed("hcanlz")
def hcanlz():
    cob = lambda typ, es: (b"PCOB", struct.pack(">IHH", typ, 0, len(es)) + b"\0" * 8, b"".join(es))
    co2 = lambda typ, es: (b"PCO2", struct.pack(">IHH", typ, len(es), 0), b"".join(es))
    yield "dat-split", anlz_file([cob(1, [pcpt(3, 300), pcpt(1, 100)]), cob(1, [pcpt(5, 500)]),
                                  cob(0, [pcpt(0, 7)])])
    yield "ext-colored", anlz_file([cob(1, [pcpt(4, 400)]), co2(1, [pcp2(1, 25399, 0xff0017),
                                    pcp2(2, 29343, 0x00c4ff, b"\0d\0r\0o\0p"), pcp2(8, 999999, 0xffe800)])])
    yield "no-cues", anlz_file([(b"PPTH", b"\0" * 4, b"\0" * 8)])
    yield "short", b"PMAI\0\0\0\x1c"


@seed("cmbrowse")
def cmbrowse():
    mods = (b"Echo Out|audio|2019-09-14|1|1|1840\n"
            b"Beat Counter|visual|2019-10-12|1|1|12\n"
            b"Auto Gain|controls|2019-10-18|0|1|0\n"
            b"Old Deck|controls|2019-10-19|0|0|5000\n"
            b"Mystery|||0|1\n"
            b"Echo Out|audio|2019-09-14|0|1|1e3")
    yield "all-az", b"\0" + mods
    yield "effects-updated", b"\x0f" + mods
    yield "installed", b"\x1e" + mods
    yield "unknown-sort", b"\xff" + mods
    yield "bad-values", (b"\x14"
                         b"A|Visual|2019-13-01|1|1\nB|visuals|2019-10-190|0|1\n"
                         b"C|synths|2019-10-1|0|1\nD|audio|2019/10/19|0|1\n"
                         b"E|tools|2019-02-31|1|0\nF|\xc3\xa9|0000-01-01")
    yield "no-mods", b"\x05"
    yield "downloads", b"\x29" + mods
    yield "bad-counts", (b"\x28"
                         b"A|audio|2019-01-01|0|1|9999999999999\nB|audio|2019-01-02|0|1|99999999999999\n"
                         b"C|audio|2019-01-03|0|1|-1\nD|audio|2019-01-04|0|1| 7\nE|audio||0|1|007")


def main():
    names = sys.argv[1:] or sorted(SEEDS)
    for n in names:
        d = os.path.join(ROOT, n)
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            if not f.startswith("regress-"):
                os.unlink(os.path.join(d, f))
        for fname, data in SEEDS[n]():
            with open(os.path.join(d, fname), "wb") as fh:
                fh.write(data)


if __name__ == "__main__":
    main()
