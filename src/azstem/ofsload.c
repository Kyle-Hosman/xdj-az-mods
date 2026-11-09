/* SPDX-License-Identifier: GPL-3.0-only */
/* ---- Stem offset, measured at load ------------------------------------
 * Included by azstem.c after the shared correlator (ofs_correlate).
 *
 * A track's stem offset is the sum of two delays:
 *   E  EP147's: MP3 is decoded by libmpg123 with FLAGS 0x300 (gapless off) from
 *      the top of the file. Playback starts one MPEG frame later still when the
 *      Info/Xing frame has the same channel mode as the first audio frame, and at
 *      other than 44.1 kHz EP147's resampler adds its own delay;
 *   P  the stem set's own delay, which varies with the app version that made it.
 * P is not recorded anywhere, so it is measured: decode the head of the source
 * the way EP147 does (this process's libmpg123, EP147's config), apply E's frame
 * rule, and cross-correlate against the stem sum.
 *
 * Measuring at load, with the RT gate held closed (g_gate_hold), means stems
 * never jump under a playing track; the cost is that they open ~1 s after load,
 * with EP147's own audio playing until then.
 *
 * AAC in MP4 works the same way (m4a_decode_native). Only sources whose rate has a
 * verified resampler delay are applied (g_ofs_cal); anything else is measured and
 * logged with what it would apply, but keeps offset 0.
 * Unknown is safe, wrong is not. Lossless sources have no decoder delay and no
 * decoder here to measure with, so their offset is the resampler delay alone. */

enum {
    MP3_CLS_NONE = 0,
    MP3_CLS_INFO,
    MP3_CLS_XING,
    MP3_CLS_NOTAG
};
static const char *mp3_cls_name(int c) {
    static const char *n[] = {"not-mp3", "Info", "Xing(VBR)", "no-Info"};
    return (c >= 0 && c <= MP3_CLS_NOTAG) ? n[c] : "?";
}

/* EP147's resampler delay against our linear one, in samples at 44.1 kHz, per source rate.
 * Verified against EP147's real audio; a rate not listed here is not applied. */
typedef struct {
    int rate;
    long cal44;
} ofs_cal_t;
static const ofs_cal_t g_ofs_cal[] = {
    {22050, 59},
    {24000, 54},
    {32000, 41},
    {44100, 0},
    {48000, 27},
    {88200, 15},
    {96000, 14},
};
static const ofs_cal_t *ofs_cal_for(int rate) {
    for (size_t i = 0; i < sizeof g_ofs_cal / sizeof g_ofs_cal[0]; i++)
        if (g_ofs_cal[i].rate == rate)
            return &g_ofs_cal[i];
    return NULL;
}

/* One Layer III frame header. */
typedef struct {
    int ver, rate, mode; /* ver: 3 = MPEG-1; mode: 0 stereo, 1 joint, 2 dual, 3 mono */
    long size;
} mp3_hdr_t;
static int mp3_hdr(const unsigned char *b, mp3_hdr_t *h) {
    static const int rates[4][3] = {
        {11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    static const short kbps[2][15] = {
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}};
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0)
        return 0;
    int ver = (b[1] >> 3) & 3, layer = (b[1] >> 1) & 3, bri = b[2] >> 4, sri = (b[2] >> 2) & 3;
    if (ver == 1 || layer != 1 || sri == 3 || bri == 15 || bri == 0)
        return 0;
    h->ver = ver;
    h->rate = rates[ver][sri];
    h->mode = b[3] >> 6;
    h->size = (ver == 3 ? 144L : 72L) * kbps[ver == 3][bri] * 1000 / h->rate + ((b[2] >> 1) & 1);
    return 1;
}
#define MP3_SCAN (256 * 1024) /* the first frame lies within this much after the ID3 tag */

/* The first MPEG audio frame after any ID3v2 tag: a Layer III header whose next frame follows it
 * at the same version and rate, so a sync pattern inside padding or junk is not taken for one.
 * Returns the class; MP3_CLS_NONE if none was found. *frame = what EP147 adds for an Info/Xing
 * frame: one frame of samples when its channel mode equals the first audio frame's, else 0. The
 * mode bits alone decide, whatever the vendor string or any other tag field says. *first = that
 * frame's offset in the file. */
static int mp3_probe(const char *path, int *rate, char *vendor, size_t vn, long *frame,
                     long *first) {
    *rate = 0;
    *frame = 0;
    *first = 0;
    if (vn)
        vendor[0] = 0;
    FILE *f = fopen(path, "rbe");
    if (!f)
        return MP3_CLS_NONE;
    unsigned char h[10];
    long skip = 0;
    if (fread(h, 1, 10, f) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
        skip = 10 + (((long) (h[6] & 0x7f) << 21) | ((long) (h[7] & 0x7f) << 14) |
                     ((long) (h[8] & 0x7f) << 7) | (long) (h[9] & 0x7f));
        if (h[5] & 0x10)
            skip += 10; /* footer present */
    }
    unsigned char *b = malloc(MP3_SCAN);
    size_t n = (b && fseek(f, skip, SEEK_SET) == 0) ? fread(b, 1, MP3_SCAN, f) : 0;
    fclose(f);
    int cls = MP3_CLS_NONE;
    for (size_t i = 0; i + 4 <= n; i++) {
        mp3_hdr_t a, c;
        if (!mp3_hdr(b + i, &a))
            continue;
        size_t next = i + (size_t) a.size;
        if (next + 4 > n || !mp3_hdr(b + next, &c) || c.ver != a.ver || c.rate != a.rate) {
            /* An Info frame with no frame right after it: whether EP147 counts it is unproven,
             * so the file has no class rather than the class of the frames behind it. */
            size_t t = i + 4 + (a.ver == 3 ? (a.mode == 3 ? 17 : 32) : (a.mode == 3 ? 9 : 17));
            if (t + 4 <= n && (!memcmp(b + t, "Info", 4) || !memcmp(b + t, "Xing", 4)))
                break;
            continue;
        }
        *rate = a.rate;
        *first = skip + (long) i;
        size_t tag = i + 4 + (a.ver == 3 ? (a.mode == 3 ? 17 : 32) : (a.mode == 3 ? 9 : 17));
        cls = MP3_CLS_NOTAG;
        if (tag + 4 > n)
            break;
        int info = !memcmp(b + tag, "Info", 4), xing = !memcmp(b + tag, "Xing", 4);
        if (!info && !xing)
            break;
        if (tag + 120 + 9 <= n && vn > 9) {
            int k = 0;
            for (; k < 9 && b[tag + 120 + k] >= 0x20 && b[tag + 120 + k] < 0x7f; k++)
                vendor[k] = (char) b[tag + 120 + k];
            vendor[k] = 0;
        }
        if (a.mode == c.mode)
            *frame = a.ver == 3 ? 1152 : 576;
        cls = xing ? MP3_CLS_XING : MP3_CLS_INFO;
        break;
    }
    free(b);
    return cls;
}

/* libmpg123 as EP147 uses it: the same library, bound through the process's own
 * symbols (EP147 imports exactly these; decode_frame is the _64 name there).
 * Constants from mpg123.h: FLAGS 1, INDEX_SIZE 15, ENC_SIGNED_16 0xd0, STEREO 2,
 * OK 0, NEW_FORMAT -11. */
static struct {
    int bound;
    void *(*new_)(const char *, int *);
    void (*del)(void *);
    int (*param)(void *, int, long, double);
    int (*fmt_none)(void *);
    int (*fmt)(void *, long, int, int);
    int (*open_feed)(void *);
    int (*feed)(void *, const unsigned char *, size_t);
    int (*decode_frame)(void *, long long *, unsigned char **, size_t *);
} g_m123;
static int m123_bind(void) {
    if (g_m123.bound)
        return g_m123.bound > 0;
    *(void **) &g_m123.new_ = dlsym(RTLD_DEFAULT, "mpg123_new");
    *(void **) &g_m123.del = dlsym(RTLD_DEFAULT, "mpg123_delete");
    *(void **) &g_m123.param = dlsym(RTLD_DEFAULT, "mpg123_param");
    *(void **) &g_m123.fmt_none = dlsym(RTLD_DEFAULT, "mpg123_format_none");
    *(void **) &g_m123.fmt = dlsym(RTLD_DEFAULT, "mpg123_format");
    *(void **) &g_m123.open_feed = dlsym(RTLD_DEFAULT, "mpg123_open_feed");
    *(void **) &g_m123.feed = dlsym(RTLD_DEFAULT, "mpg123_feed");
    *(void **) &g_m123.decode_frame = dlsym(RTLD_DEFAULT, "mpg123_decode_frame_64");
    if (!g_m123.decode_frame)
        *(void **) &g_m123.decode_frame = dlsym(RTLD_DEFAULT, "mpg123_decode_frame");
    g_m123.bound = (g_m123.new_ && g_m123.del && g_m123.param && g_m123.fmt_none && g_m123.fmt &&
                    g_m123.open_feed && g_m123.feed && g_m123.decode_frame)
                       ? 1
                       : -1;
    if (g_m123.bound < 0)
        logln("STEMOFS-LOAD: libmpg123 symbols not found in the process -- load-time offset "
              "disabled");
    return g_m123.bound > 0;
}

/* Resample a native-rate mono decode to 44100 (linear) behind `cal44` zeros at
 * 44.1k (the difference between EP147's resampler and this one). */
static long ofs_to44(const float *nat, long got, int rate, long cal44, float *out, long maxn) {
    long n = 0;
    for (; n < cal44 && n < maxn; n++)
        out[n] = 0.f;
    if (rate == AZ_EP147_RATE) {
        for (long i = 0; i < got && n < maxn; i++)
            out[n++] = nat[i];
        return n;
    }
    for (; n < maxn; n++) {
        double p = (double) (n - cal44) * rate / AZ_EP147_RATE;
        long i0 = (long) p;
        if (i0 + 1 >= got)
            break;
        out[n] = nat[i0] + (float) (p - i0) * (nat[i0 + 1] - nat[i0]);
    }
    return n;
}
static long ofs_need_native(long maxn, int rate, long cal44) {
    long need = (long) ((double) (maxn - cal44) * rate / AZ_EP147_RATE) + 4;
    return need < 0 ? 0 : need;
}

/* MP3 the way EP147 decodes it: `frame` zeros first at the file's rate, then
 * libmpg123 in EP147's config, fed from the first frame (`first`), since what lies before it can
 * make the decoder lose a frame. Native-rate mono; returns a malloc'd buffer of *got samples, or
 * NULL. */
static float *mp3_decode_native(const char *path, int rate, long frame, long first, long need,
                                long *got_out) {
    *got_out = 0;
    if (!m123_bind() || rate <= 0)
        return NULL;
    FILE *f = fopen(path, "rbe");
    if (!f)
        return NULL;
    if (fseek(f, first, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    int err = 0;
    void *h = g_m123.new_(NULL, &err);
    if (!h) {
        fclose(f);
        return NULL;
    }
    g_m123.param(h, 0 /*VERBOSE*/, 0, 0);
    g_m123.param(h, 1 /*FLAGS*/, 0x300, 0); /* SEEKBUFFER|FUZZY: gapless off, as EP147 */
    g_m123.param(h, 15 /*INDEX_SIZE*/, -1, 0);
    g_m123.fmt_none(h);
    g_m123.fmt(h, rate, 2 /*STEREO*/, 0xd0 /*ENC_SIGNED_16*/);
    g_m123.open_feed(h);
    if (need < frame)
        need = frame;
    float *nat = malloc((size_t) (need + 1) * sizeof(float));
    long got = 0;
    if (nat)
        for (; got < frame; got++)
            nat[got] = 0.f;
    unsigned char in[16384];
    size_t rd;
    while (nat && got < need && (rd = fread(in, 1, sizeof in, f)) > 0) {
        g_m123.feed(h, in, rd);
        for (;;) {
            long long num;
            unsigned char *aud;
            size_t bytes;
            int r = g_m123.decode_frame(h, &num, &aud, &bytes);
            if (r == -11)
                continue; /* NEW_FORMAT */
            if (r != 0)
                break; /* NEED_MORE, DONE or error */
            const short *s = (const short *) aud;
            for (size_t i = 0; i + 1 < bytes / 2 && got < need; i += 2)
                nat[got++] = (s[i] + s[i + 1]) / 65536.f;
        }
    }
    g_m123.del(h);
    fclose(f);
    *got_out = got;
    return nat;
}

/* ---- AAC in MP4 (.m4a) -------------------------------------------------------
 * EP147 plays AAC raw: encoder priming is not trimmed (its delay
 * equals the priming: afconvert 2112, ffmpeg 1024; ffmpeg at 48k gives 968 = 1024
 * resampled, plus the same +27 as MP3). So the model is a raw decode from the
 * first access unit, using the fdk-aac already loaded in EP147's process. EP147
 * decodes AAC with its own built-in decoder, so equivalence is verified per rule
 * by source rate (g_ofs_cal), not assumed. The MP4 parsing is the minimum needed to walk one track's sample table. */
static struct {
    int bound;
    void *(*open)(int, unsigned);
    int (*config_raw)(void *, unsigned char **, const unsigned *);
    int (*fill)(void *, unsigned char **, const unsigned *, unsigned *);
    int (*decode)(void *, short *, int, unsigned);
    const int *(*info)(void *); /* CStreamInfo: sampleRate, frameSize, numChannels first */
    int (*set_param)(void *, int, int);
    void (*close)(void *);
} g_fdk;
/* CStreamInfo (fdk-aac 2.0.2 aacdecoder_lib.h, LP64): three INTs, two pointers at
 * +16/+24, then INTs; outputDelay ("samples the output is additionally delayed
 * by the decoder") is int index 17. */
#define FDK_SI_OUTPUT_DELAY 17
static int fdk_bind(void) {
    if (g_fdk.bound)
        return g_fdk.bound > 0;
    *(void **) &g_fdk.open = dlsym(RTLD_DEFAULT, "aacDecoder_Open");
    *(void **) &g_fdk.config_raw = dlsym(RTLD_DEFAULT, "aacDecoder_ConfigRaw");
    *(void **) &g_fdk.fill = dlsym(RTLD_DEFAULT, "aacDecoder_Fill");
    *(void **) &g_fdk.decode = dlsym(RTLD_DEFAULT, "aacDecoder_DecodeFrame");
    *(void **) &g_fdk.info = dlsym(RTLD_DEFAULT, "aacDecoder_GetStreamInfo");
    *(void **) &g_fdk.set_param = dlsym(RTLD_DEFAULT, "aacDecoder_SetParam");
    *(void **) &g_fdk.close = dlsym(RTLD_DEFAULT, "aacDecoder_Close");
    g_fdk.bound = (g_fdk.open && g_fdk.config_raw && g_fdk.fill && g_fdk.decode && g_fdk.info &&
                   g_fdk.set_param && g_fdk.close)
                      ? 1
                      : -1;
    if (g_fdk.bound < 0)
        logln("STEMOFS-LOAD: fdk-aac symbols not found in the process -- AAC offset disabled");
    return g_fdk.bound > 0;
}
static uint32_t be32(const unsigned char *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}
static uint64_t be64(const unsigned char *p) {
    return ((uint64_t) be32(p) << 32) | be32(p + 4);
}
/* The idx-th child box of `type` in [p, p+n): its payload, and its length in *len. */
static const unsigned char *mp4_child(const unsigned char *p, size_t n, const char *type, int idx,
                                      size_t *len) {
    size_t off = 0;
    while (off + 8 <= n) {
        uint64_t sz = be32(p + off);
        size_t hdr = 8;
        if (sz == 1) {
            if (off + 16 > n)
                return NULL;
            sz = be64(p + off + 8);
            hdr = 16;
        } else if (sz == 0)
            sz = n - off;
        if (sz < hdr || sz > n - off)
            return NULL;
        if (!memcmp(p + off + 4, type, 4) && idx-- == 0) {
            *len = (size_t) sz - hdr;
            return p + off + hdr;
        }
        off += (size_t) sz;
    }
    return NULL;
}
static unsigned char *mp4_read_moov(FILE *f, size_t *len) {
    unsigned char h[16];
    off_t at = 0;
    for (int guard = 0; guard < 64; guard++) {
        if (fseeko(f, at, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8)
            return NULL;
        uint64_t sz = be32(h);
        size_t hdr = 8;
        if (sz == 1) {
            if (fread(h + 8, 1, 8, f) != 8)
                return NULL;
            sz = be64(h + 8);
            hdr = 16;
        }
        if (sz < hdr)
            return NULL;
        if (!memcmp(h + 4, "moov", 4)) {
            if (sz - hdr > (64u << 20))
                return NULL;
            unsigned char *m = malloc((size_t) (sz - hdr));
            if (!m || fread(m, 1, (size_t) (sz - hdr), f) != sz - hdr) {
                free(m);
                return NULL;
            }
            *len = (size_t) (sz - hdr);
            return m;
        }
        at += (off_t) sz;
    }
    return NULL;
}
/* AudioSpecificConfig from an esds payload. */
static int esds_asc(const unsigned char *p, size_t n, unsigned char *asc, unsigned *alen) {
    size_t i = 4; /* version/flags */
    while (i + 2 <= n) {
        int tag = p[i++];
        uint32_t len = 0;
        for (int k = 0; k < 4 && i < n; k++) {
            unsigned char b = p[i++];
            len = (len << 7) | (b & 0x7f);
            if (!(b & 0x80))
                break;
        }
        if (tag == 3) { /* ES_Descriptor: descend */
            if (i + 3 > n)
                return 0;
            int fl = p[i + 2];
            i += 3;
            if (fl & 0x80)
                i += 2;
            if ((fl & 0x40) && i < n)
                i += 1 + p[i];
            if (fl & 0x20)
                i += 2;
            continue;
        }
        if (tag == 4) {
            i += 13;
            continue;
        } /* DecoderConfigDescriptor: descend */
        if (tag == 5) {
            if (len == 0 || len > 64 || i + len > n)
                return 0;
            memcpy(asc, p + i, len);
            *alen = len;
            return 1;
        }
        i += len;
    }
    return 0;
}
/* The first 'soun' track of `path`, decoded raw from its first access unit to
 * native-rate mono. *rate = the decoder's output rate, *aot = the ASC's object
 * type; `desc` gets a short description (tool, edit list, iTunSMPB) for the log. */
static float *m4a_decode_native(const char *path, long maxn44, int *rate, int *aot, int *sbr,
                                char *desc, size_t dn, long *got_out) {
    *got_out = 0;
    *rate = 0;
    *aot = 0;
    *sbr = 0;
    if (dn)
        desc[0] = 0;
    FILE *f = fopen(path, "rbe");
    if (!f)
        return NULL;
    size_t mlen = 0;
    unsigned char *moov = mp4_read_moov(f, &mlen);
    float *nat = NULL;
    void *h = NULL;
    unsigned char *au = NULL;
    short *pcm = NULL;
    enum {
        PCM_MAX = 2048 * 8
    }; /* per call: two decks can load at once */
    if (!moov)
        goto done;
    const unsigned char *stbl = NULL, *edts = NULL;
    size_t stbl_n = 0, edts_n = 0;
    for (int t = 0;; t++) {
        size_t tn, mn, hn, in_, sn;
        const unsigned char *trak = mp4_child(moov, mlen, "trak", t, &tn);
        if (!trak)
            break;
        const unsigned char *mdia = mp4_child(trak, tn, "mdia", 0, &mn);
        if (!mdia)
            continue;
        const unsigned char *hdlr = mp4_child(mdia, mn, "hdlr", 0, &hn);
        if (!hdlr || hn < 12 || memcmp(hdlr + 8, "soun", 4))
            continue;
        const unsigned char *minf = mp4_child(mdia, mn, "minf", 0, &in_);
        if (!minf)
            continue;
        stbl = mp4_child(minf, in_, "stbl", 0, &sn);
        stbl_n = sn;
        edts = mp4_child(trak, tn, "edts", 0, &edts_n);
        break;
    }
    if (!stbl)
        goto done;
    size_t dl, zl, cl, ol;
    const unsigned char *stsd = mp4_child(stbl, stbl_n, "stsd", 0, &dl);
    const unsigned char *stsz = mp4_child(stbl, stbl_n, "stsz", 0, &zl);
    const unsigned char *stsc = mp4_child(stbl, stbl_n, "stsc", 0, &cl);
    const unsigned char *stco = mp4_child(stbl, stbl_n, "stco", 0, &ol);
    int co64 = 0;
    if (!stco) {
        stco = mp4_child(stbl, stbl_n, "co64", 0, &ol);
        co64 = 1;
    }
    if (!stsd || dl < 16 || !stsz || zl < 12 || !stsc || cl < 8 || !stco || ol < 8)
        goto done;
    size_t en;
    const unsigned char *mp4a = mp4_child(stsd + 8, dl - 8, "mp4a", 0, &en);
    if (!mp4a || en < 28)
        goto done;
    unsigned ver = (mp4a[8] << 8) | mp4a[9]; /* QuickTime sound description version */
    size_t kids = 28 + (ver == 1 ? 16 : ver == 2 ? 36
                                                 : 0);
    if (kids > en)
        goto done;
    size_t esn;
    const unsigned char *esds = mp4_child(mp4a + kids, en - kids, "esds", 0, &esn);
    unsigned char asc[64];
    unsigned alen = 0;
    if (!esds || !esds_asc(esds, esn, asc, &alen) || alen < 2)
        goto done;
    *aot = asc[0] >> 3;
    /* description: encoder tool, edit-list media_time, iTunSMPB present */
    long long mt = -1;
    size_t eln;
    const unsigned char *elst = edts ? mp4_child(edts, edts_n, "elst", 0, &eln) : NULL;
    if (elst && eln >= 8 && be32(elst + 4) >= 1)
        mt = elst[0] == 1 ? (eln >= 24 ? (long long) be64(elst + 16) : -1)
                          : (eln >= 16 ? (long long) (int32_t) be32(elst + 12) : -1);
    char tool[16] = "";
    for (size_t i = 0; i + 24 < mlen; i++)
        if (moov[i] == 0xA9 && !memcmp(moov + i + 1, "too", 3) &&
            !memcmp(moov + i + 8, "data", 4)) {
            uint32_t ds = be32(moov + i + 4);
            size_t k = 0;
            for (; k + 16 < ds && k < sizeof tool - 1 && i + 16 + k < mlen &&
                   moov[i + 16 + k] >= 0x20 && moov[i + 16 + k] < 0x7f;
                 k++)
                tool[k] = (char) moov[i + 16 + k];
            tool[k] = 0;
            break;
        }
    int smpb = 0;
    for (size_t i = 0; i + 8 <= mlen; i++)
        if (!memcmp(moov + i, "iTunSMPB", 8)) {
            smpb = 1;
            break;
        }
    snprintf(desc, dn, "%s%sedts=%lld smpb=%d", tool, tool[0] ? " " : "", mt, smpb);

    if (!fdk_bind())
        goto done;
    h = g_fdk.open(0 /*TT_MP4_RAW*/, 1);
    if (!h)
        goto done;
    {
        unsigned char *ap = asc;
        unsigned al = alen;
        if (g_fdk.config_raw(h, &ap, &al) != 0)
            goto done;
    }
    /* fdk-aac 2.x delays output by default: energy-interpolation concealment holds
     * one frame (1024) and the limiter looks ahead 15 ms (661 @44.1k, 720 @48k).
     * Turn both off (neither changes a clean decode) and drop whatever outputDelay
     * the decoder still reports. */
    g_fdk.set_param(h, 0x0100 /*AAC_CONCEAL_METHOD*/, 0);
    g_fdk.set_param(h, 0x0004 /*AAC_PCM_LIMITER_ENABLE*/, 0);
    long skip = -1;
    uint32_t fixed = be32(stsz + 4), nsamp = be32(stsz + 8);
    uint32_t nstsc = be32(stsc + 4), nchunk = be32(stco + 4);
    if ((!fixed && zl < 12 + (size_t) nsamp * 4) || cl < 8 + (size_t) nstsc * 12 ||
        ol < 8 + (size_t) nchunk * (co64 ? 8 : 4) || nstsc == 0)
        goto done;
    long need = 0, got = 0;
    int errs = 0;
    au = malloc(1 << 16);
    pcm = malloc(PCM_MAX * sizeof(short));
    if (!au || !pcm)
        goto done;
    uint32_t s = 0, ei = 0;
    for (uint32_t c = 0; c < nchunk && s < nsamp && errs < 16; c++) {
        while (ei + 1 < nstsc && be32(stsc + 8 + (ei + 1) * 12) <= c + 1)
            ei++;
        uint32_t spc = be32(stsc + 8 + ei * 12 + 4);
        uint64_t off = co64 ? be64(stco + 8 + (size_t) c * 8) : be32(stco + 8 + (size_t) c * 4);
        for (uint32_t k = 0; k < spc && s < nsamp && errs < 16; k++, s++) {
            uint32_t sz = fixed ? fixed : be32(stsz + 12 + (size_t) s * 4);
            if (sz == 0 || sz > (1 << 16) || fseeko(f, (off_t) off, SEEK_SET) != 0 ||
                fread(au, 1, sz, f) != sz) {
                errs = 99;
                break;
            }
            off += sz;
            unsigned char *bp = au;
            unsigned bs = sz, valid = sz;
            g_fdk.fill(h, &bp, &bs, &valid);
            int e = g_fdk.decode(h, pcm, PCM_MAX, 0);
            if (e == 0x1002)
                continue; /* NOT_ENOUGH_BITS */
            if (e != 0) {
                errs++;
                continue;
            }
            const int *si = g_fdk.info(h);
            int sr = si[0], fs = si[1], ch = si[2];
            if (sr <= 0 || fs <= 0 || ch <= 0 || fs * ch > PCM_MAX) {
                errs = 99;
                break;
            }
            if (!nat) {
                *rate = sr;
                need = ofs_need_native(maxn44, sr, 0) + 64;
                nat = malloc((size_t) need * sizeof(float));
                if (!nat) {
                    errs = 99;
                    break;
                }
            }
            if (sr != *rate) {
                errs = 99;
                break;
            }
            if (skip < 0) {
                skip = (long) (unsigned) si[FDK_SI_OUTPUT_DELAY];
                size_t dl0 = strlen(desc);
                if (dl0 < dn)
                    snprintf(desc + dl0, dn - dl0, " od=%ld", skip);
            }
            for (int i = 0; i < fs && got < need; i++) {
                if (skip > 0) {
                    skip--;
                    continue;
                }
                nat[got++] =
                    ch > 1 ? (pcm[i * ch] + pcm[i * ch + 1]) / 65536.f : pcm[i * ch] / 32768.f;
            }
            if (got >= need)
                goto enough;
        }
    }
enough:
    /* implicit SBR: the ASC says one rate, the decoder outputs twice it */
    {
        static const int fr[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                   22050, 16000, 12000, 11025, 8000, 7350};
        int fi = ((asc[0] & 7) << 1) | (asc[1] >> 7);
        if (*aot == 5 || *aot == 29 || (fi < 13 && *rate && *rate != fr[fi]))
            *sbr = 1;
    }
    *got_out = got;
done:
    if (h)
        g_fdk.close(h);
    free(au);
    free(pcm);
    free(moov);
    fclose(f);
    if (!*got_out) {
        free(nat);
        return NULL;
    }
    return nat;
}
/* ---- Lossless sources: only their sample rate matters ---------------------------
 * FLAC, WAV and AIFF have no decoder delay, and EP147 decodes them with code this process cannot
 * call, so the offset is the resampler delay for the file's rate. Rate read from the file's own
 * header; 0 = not a header this understands. */
static uint32_t ofs_le32(const unsigned char *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}
/* The rate in an AIFF COMM chunk's 80-bit extended float. */
static int aiff_rate(const unsigned char *x) {
    int e = ((x[0] & 0x7f) << 8) | x[1];
    int sh = 16383 + 31 - e; /* the mantissa's top 32 bits, an integer once shifted */
    uint32_t m = be32(x + 2);
    if ((x[0] & 0x80) || sh < 0 || sh > 31)
        return 0;
    return (int) (m >> sh);
}
static int lossless_rate(const char *path) {
    FILE *f = fopen(path, "rbe");
    if (!f)
        return 0;
    unsigned char h[64];
    int rate = 0;
    size_t n = fread(h, 1, 12, f);
    if (n == 12 && !memcmp(h, "fLaC", 4)) {
        /* STREAMINFO comes first: 4 block header + 10 + 20 bits of rate */
        if (fread(h + 12, 1, 10, f) == 10)
            rate = (h[18] << 12) | (h[19] << 4) | (h[20] >> 4);
    } else if (n == 12 && ((!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) ||
                           (!memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) ||
                                                      !memcmp(h + 8, "AIFC", 4))))) {
        int wav = h[0] == 'R';
        off_t at = 12;
        for (int guard = 0; guard < 256 && !rate; guard++) {
            unsigned char c[26];
            if (fseeko(f, at, SEEK_SET) != 0 || fread(c, 1, 8, f) != 8)
                break;
            uint32_t sz = wav ? ofs_le32(c + 4) : be32(c + 4);
            if (wav && !memcmp(c, "fmt ", 4)) {
                if (fread(c + 8, 1, 8, f) == 8)
                    rate = (int) ofs_le32(c + 12);
                break;
            }
            if (!wav && !memcmp(c, "COMM", 4)) {
                if (fread(c + 8, 1, 18, f) == 18)
                    rate = aiff_rate(c + 16);
                break;
            }
            at += (off_t) sz + 8 + (sz & 1); /* chunks are padded to even */
        }
    }
    fclose(f);
    return rate > 0 && rate <= 768000 ? rate : 0;
}
/* The rate of an MP4's ALAC track from its sample entry, 0 if it has none. A 16.16 field, so only
 * rates below 65536 are readable here; a higher rate reads as another rate or 0 and stays at offset 0
 * unless the truncated value happens to be listed, which no listed rate is. */
static int m4a_alac_rate(const char *path) {
    FILE *f = fopen(path, "rbe");
    if (!f)
        return 0;
    size_t mlen = 0;
    unsigned char *moov = mp4_read_moov(f, &mlen);
    fclose(f);
    int rate = 0;
    for (int t = 0; moov && !rate; t++) {
        size_t tn, n1, n2, n3, n4, en;
        const unsigned char *p = mp4_child(moov, mlen, "trak", t, &tn);
        if (!p)
            break;
        if (!(p = mp4_child(p, tn, "mdia", 0, &n1)) || !(p = mp4_child(p, n1, "minf", 0, &n2)) ||
            !(p = mp4_child(p, n2, "stbl", 0, &n3)) || !(p = mp4_child(p, n3, "stsd", 0, &n4)) ||
            n4 < 8)
            continue;
        const unsigned char *alac = mp4_child(p + 8, n4 - 8, "alac", 0, &en);
        if (alac && en >= 28)
            rate = (alac[24] << 8) | alac[25];
    }
    free(moov);
    return rate;
}

/* Frames decoded so far by every present stem (the gate's own min), ignoring the hold. */
static long ofs_stems_decoded(int deck) {
    unsigned present = atomic_load_explicit(&g_stem_present[deck], memory_order_relaxed);
    long m = -1;
    for (int k = 0; k < NSTEM; k++)
        if (present & (1u << k)) {
            long d = atomic_load_explicit(&g_stem_dec[deck][k], memory_order_relaxed);
            if (m < 0 || d < m)
                m = d;
        }
    return m < 0 ? 0 : m;
}

#define OFSL_HEAD ((long) AZ_EP147_RATE * 20) /* second pass: the first 20 s of the source */
#define OFSL_LONG ((long) AZ_EP147_RATE * 90) /* last pass, for a long quiet start */
#define OFSL_EARLY ((long) AZ_EP147_RATE * 6) /* first pass: a window starting in the first 6 s */
#define OFSL_WAITMS 30000                     /* give up waiting for the stems after 30 s */

/* ---- Offset cache: <stems folder>/azstem.ofs ----------------------------------
 * One line, "AZOFS1 <source size> <source mtime> <stems key> <lag>\n", written only
 * for an applied measurement. A repeat load of the same source with the same stem
 * files opens the gate without measuring. Any other content, or any key that differs,
 * is a miss and the load measures as usual. Bump the magic whenever the frame rule or
 * g_ofs_cal change, so no lag measured under older rules is reused. */
#define OFSC_MAGIC "AZOFS2"
static void ofs_cache_path(int deck, char *out, size_t n) {
    snprintf(out, n, "%s/azstem.ofs", g_env_dir[deck]);
}
/* One decimal field of a cache line, then exactly one space (or, for the last field, the
 * newline that ends the line). 0 = anything else, including a value out of range. */
static int ofsc_field(const char **p, int last, int is_unsigned, long long *v,
                      unsigned long long *u) {
    const char *s = *p;
    char *e;
    if (!isdigit((unsigned char) s[0]) &&
        !(s[0] == '-' && !is_unsigned && isdigit((unsigned char) s[1])))
        return 0;
    errno = 0;
    if (is_unsigned)
        *u = strtoull(s, &e, 10);
    else
        *v = strtoll(s, &e, 10);
    if (errno || e == s || *e != (last ? '\n' : ' '))
        return 0;
    *p = e + 1;
    return 1;
}
/* 1 and *lag when `line` is a cache line for exactly this source and stem set. */
static int ofs_cache_parse(const char *line, long long size, long long mtime,
                           unsigned long long skey, long *lag) {
    static const char magic[] = OFSC_MAGIC " ";
    if (strncmp(line, magic, sizeof magic - 1) != 0)
        return 0;
    const char *p = line + sizeof magic - 1;
    long long sz, mt, l;
    unsigned long long k, unused;
    if (!ofsc_field(&p, 0, 0, &sz, &unused) || !ofsc_field(&p, 0, 0, &mt, &unused) ||
        !ofsc_field(&p, 0, 1, &l, &k) || !ofsc_field(&p, 1, 0, &l, &unused) || *p != '\0')
        return 0;
    if (sz != size || mt != mtime || k != skey || l < -OFS_LAGMAX || l > OFS_LAGMAX)
        return 0;
    *lag = (long) l;
    return 1;
}
static int ofs_cache_load(int deck, long long size, long long mtime, unsigned long long skey,
                          long *lag) {
    if (!g_env_dir[deck][0])
        return 0;
    char path[1152], line[128];
    ofs_cache_path(deck, path, sizeof path);
    FILE *f = fopen(path, "re");
    if (!f)
        return 0;
    int ok = fgets(line, sizeof line, f) != NULL && ofs_cache_parse(line, size, mtime, skey, lag);
    fclose(f);
    return ok;
}
static void ofs_cache_save(int deck, long long size, long long mtime, unsigned long long skey,
                           long lag) {
    if (!g_env_dir[deck][0])
        return;
    char path[1152], tmp[1160];
    ofs_cache_path(deck, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path); /* the stick can be pulled: whole or not at all */
    FILE *f = fopen(tmp, "we");
    if (!f)
        return;
    int ok = fprintf(f, OFSC_MAGIC " %lld %lld %llu %ld\n", size, mtime, skey, lag) > 0;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0)
        unlink(tmp);
}

/* The source head the way EP147 decodes it, resampled to 44.1k, into x[maxn].
 * Returns the frames written; 0 with *why set when nothing usable came out. */
static long ofs_source_head(const char *src, int is_mp3, long maxn, float *x, int *rate,
                            char *vendor, size_t vn, char *kind, size_t kn, int *proven,
                            const char **why, int deck) {
    float *nat = NULL;
    long got = 0, n = 0;
    *proven = 0;
    if (is_mp3) {
        long frame = 0, first = 0;
        int cls = mp3_probe(src, rate, vendor, vn, &frame, &first);
        snprintf(kind, kn, "MP3 %s", mp3_cls_name(cls));
        if (cls == MP3_CLS_NONE) {
            *why = "no Layer III frame found";
            return 0;
        }
        const ofs_cal_t *cal = ofs_cal_for(*rate);
        long c44 = cal ? cal->cal44 : 0;
        nat = mp3_decode_native(src, *rate, frame, first, ofs_need_native(maxn, *rate, c44), &got);
        if (nat)
            n = ofs_to44(nat, got, *rate, c44, x, maxn);
        *proven = cal != NULL;
    } else {
        int aot = 0, sbr = 0;
        char desc[64];
        nat = m4a_decode_native(src, maxn, rate, &aot, &sbr, desc, sizeof desc, &got);
        snprintf(kind, kn, "M4A aot=%d%s", aot, sbr ? " SBR" : "");
        snprintf(vendor, vn, "%.15s", desc);
        logln("STEMOFS-LOAD d%d   m4a: %s", deck, desc);
        if (!nat) {
            *why = "m4a: no decodable AAC track";
            return 0;
        }
        /* HE-AAC is resampled from its half-rate core, whatever rate it is decoded to */
        const ofs_cal_t *cal = (aot == 2 || sbr) ? ofs_cal_for(sbr ? *rate / 2 : *rate) : NULL;
        n = ofs_to44(nat, got, *rate, cal ? cal->cal44 : 0, x, maxn);
        *proven = cal != NULL;
    }
    free(nat);
    if (n < OFS_N + 2 * OFS_LAGMAX) {
        *why = "source decode too short";
        return 0;
    }
    return n;
}

/* Correlates the loudest OFS_N window that starts at or before `blim` against the stem
 * sum, once every stem is decoded past it. 1 = *rr holds a result (confident or not);
 * 0 = no result, *why says why. */
static int ofs_try(int deck, const float *x, long n, long blim, long t0, long long *base,
                   ofs_res_t *rr, long *waitms, const char **why) {
    deck_stems *dk = &g_dk[deck];
    /* the loudest OFS_N window: silence or a fade-in correlates with anything */
    double be = -1;
    *base = -1;
    for (long b = OFS_LAGMAX; b + OFS_N <= n && b <= blim; b += 22050) {
        double e = 0;
        for (long i = 0; i < OFS_N; i += 4)
            e += (double) x[b + i] * x[b + i];
        if (e > be) {
            be = e;
            *base = b;
        }
    }
    if (*base < 0 || be / (OFS_N / 4) < 1e-6) {
        *why = "source head is silent";
        *base = -1;
        return 0;
    }
    /* wait for every stem to be decoded past the search window */
    long want = *base + OFS_N + OFS_LAGMAX + 64, w0 = ms_now();
    for (;;) {
        if (atomic_load_explicit(&g_load_stale[deck], memory_order_acquire)) {
            *why = "track replaced during load";
            return 0;
        }
        if (ofs_stems_decoded(deck) >= want)
            break;
        if (!atomic_load_explicit(&g_stem_present[deck], memory_order_relaxed)) {
            *why = "no stem decoded";
            return 0;
        }
        if (ms_now() - t0 > OFSL_WAITMS) {
            *why = "stems not decoded in time";
            return 0;
        }
        usleep(20000);
    }
    *waitms += ms_now() - w0;
    ofs_correlate(dk, want, x + *base, *base, rr);
    return 1;
}

/* Loader thread, while this load's stem decode threads run. Measures and sets
 * dk->stem_off, then opens the gate. Every path ends in the release at `out`.
 * The gate stays closed for as long as this runs, so a stem toggled meanwhile is not
 * heard until it returns: a cached result opens it at once, and the first pass looks
 * only at the first seconds so that it needs only the first seconds of decoded stems.
 * The later passes, over 20 s and then 90 s of the source, run only when the one
 * before finds no confident peak. `skey` identifies the stem files (stems_key). */
static void ofs_load_measure(int deck, const char *track, const char *src,
                             unsigned long long skey) {
    deck_stems *dk = &g_dk[deck];
    long t0 = ms_now(), waitms = 0;
    char vendor[16] = "", kind[64] = "not-mp3/m4a";
    int rate = 0, proven = 0, pass = 0, cached = 0;
    float *x = NULL;
    const char *why = NULL;
    long lag = 0;
    double fine = 0, coarse = 0, second = 0;
    long long base = -1, ssize = -1, smtime = -1;
    int apply = 0;

    if (!src || !src[0]) {
        why = "no source path";
        goto out;
    }
    const char *dot = strrchr(src, '.');
    int is_mp3 = dot && !strcasecmp(dot, ".mp3");
    int is_m4a = dot && (!strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".mp4"));
    if (!is_mp3 && !is_m4a) {
        rate = lossless_rate(src);
        snprintf(kind, sizeof kind, "lossless");
        const ofs_cal_t *cal = ofs_cal_for(rate);
        if (!cal) {
            why = rate ? "lossless: no verified resampler delay for this rate"
                       : "not mp3/m4a, or no sample rate in its header";
            goto out;
        }
        if (dev_flag(NOSTEMOFS_PATH)) {
            why = "azstem.nostemofs -- logged, not applied";
            goto out;
        }
        lag = cal->cal44;
        apply = 1;
        goto out;
    }
    if (is_m4a) {
        int ar = m4a_alac_rate(src);
        if (ar) {
            rate = ar;
            snprintf(kind, sizeof kind, "ALAC");
            const ofs_cal_t *cal = ofs_cal_for(rate);
            if (!cal) {
                why = "lossless: no verified resampler delay for this rate";
                goto out;
            }
            if (dev_flag(NOSTEMOFS_PATH)) {
                why = "azstem.nostemofs -- logged, not applied";
                goto out;
            }
            lag = cal->cal44;
            apply = 1;
            goto out;
        }
    }
    {
        struct stat st;
        if (stat(src, &st) == 0) {
            ssize = (long long) st.st_size;
            smtime = (long long) st.st_mtime;
        }
    }
    if (ssize >= 0 && skey && !dev_flag(NOSTEMOFS_PATH) &&
        ofs_cache_load(deck, ssize, smtime, skey, &lag)) {
        snprintf(kind, sizeof kind, "cached");
        cached = apply = 1;
        goto out;
    }
    x = malloc((size_t) OFSL_LONG * sizeof(float));
    if (!x) {
        why = "no memory";
        goto out;
    }
    static const long head[3] = {OFSL_EARLY + OFS_N + OFS_LAGMAX, OFSL_HEAD, OFSL_LONG};
    static const long blim[3] = {OFSL_EARLY, OFSL_HEAD, OFSL_LONG};
    ofs_res_t rr = {0};
    int have = 0;
    for (pass = 1; pass <= 3; pass++) {
        why = NULL;
        long n = ofs_source_head(src, is_mp3, head[pass - 1], x, &rate, vendor, sizeof vendor, kind,
                                 sizeof kind, &proven, &why, deck);
        if (!n)
            break; /* the longer head cannot decode better */
        have = ofs_try(deck, x, n, blim[pass - 1], t0, &base, &rr, &waitms, &why);
        if (!have && (atomic_load_explicit(&g_load_stale[deck], memory_order_acquire) ||
                      !atomic_load_explicit(&g_stem_present[deck], memory_order_relaxed) ||
                      ms_now() - t0 > OFSL_WAITMS))
            break;
        if (have && rr.fine >= OFS_MINCORR && rr.coarse - rr.second >= OFS_MARGIN)
            break;
        if (n < head[pass - 1])
            break; /* the source is shorter than this head: a second pass sees nothing new */
    }
    if (pass > 3)
        pass = 3;
    if (!have)
        goto out;
    lag = rr.lag;
    fine = rr.fine;
    coarse = rr.coarse;
    second = rr.second;
    if (!(fine >= OFS_MINCORR && coarse - second >= OFS_MARGIN)) {
        why = "no confident peak";
        goto out;
    }
    if (!proven) {
        why = "no proven rule for this encoder/rate yet -- logged, not applied";
        goto out;
    }
    if (dev_flag(NOSTEMOFS_PATH)) {
        why = "azstem.nostemofs -- logged, not applied";
        goto out;
    }
    apply = 1;
out:
    atomic_store_explicit(&dk->stem_off, apply ? lag : 0, memory_order_relaxed);
    g_ofs_load[deck] = apply ? lag : 0;
    logln("STEMOFS-LOAD d%d '%s' %s %dHz tag='%s' base=%lld lag=%ld (%.1f ms) fine=%.3f "
          "coarse=%.3f runnerup=%.3f -> %s [%ldms, pass %d, stems wait %ldms]",
          deck, track, kind, rate, vendor, base, lag, lag / 44.1, fine, coarse, second,
          apply ? "APPLIED" : "0 (see reason)", ms_now() - t0, pass, waitms);
    if (why)
        logln("STEMOFS-LOAD d%d   reason: %s", deck, why);
    free(x);
    /* Open the gate, unless the track was replaced meanwhile (drop_stems): then
     * the hold stays and the loader frees these buffers after its threads join. */
    pthread_mutex_lock(&g_adv_lock);
    if (!atomic_load_explicit(&g_load_stale[deck], memory_order_acquire))
        atomic_store_explicit(&g_gate_hold[deck], 0, memory_order_release);
    pthread_mutex_unlock(&g_adv_lock);
    advance_stem_frames(deck); /* publishes whatever is decoded now (no-op while held) */
    if (apply && !cached && ssize >= 0 && skey)
        ofs_cache_save(deck, ssize, smtime, skey, lag);
}
