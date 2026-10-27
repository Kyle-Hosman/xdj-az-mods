/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file drum_roll_display.c
 * @brief Drum-roll sampler pad and x-pad state, the drum-roll pad tiles and SLIP LOOP
 *        caption, and the Beat FX roll-rate row and parameter bars
 *
 * Armed from the Mod Menu, the drum-roll sampler is active only in SLIP LOOP pad mode while
 * pad 8 is latched. Pads 1-4 pick the sample and the x-pad picks the roll rate. State is
 * indexed by logical deck (0..3), and calls into azstem go through function pointers
 * resolved with dlsym at overlay init (NULL until azstem.so is loaded). The x-pad byte
 * offsets are read from settings/azmod-xpad and consumed by the bus thread.
 *
 * The display side recolors EP147's own pad tiles through their ImagesComponent art index,
 * re-asserted from the JUCE message thread, and swaps the SLIP LOOP caption by pointing
 * PAD_TITLE_TBL entry 3 at an embedded strip. Hooks on the FX name and row widgets and the
 * BeatFxParamWidget layout/paint draw the roll rate row and the VOLUME / PITCH bars. Cached
 * widget pointers are revalidated by alignment and vtable before use. The file also holds
 * the hot-path flags and per-deck attenuation state used by the waveform stem scaling
 * (kill switch settings/azmod-wavemute.off).
 */
/* ---- drum-roll sampler ----------------
 * Pad mode and pads belong to the physical side; arm state belongs to the deck. */
static int g_pad_mode_slip[AZMOD_STEM_DECKS] = {0}; /* SLIP LOOP is the mode */
static _Atomic int g_drum_arm = 0;                  /* mod 6 enabled */
/* Indexed by logical deck (0..3), never by side: a side switched to deck 3/4
 * must arm that deck. azstem mixes by the same 0..3 slot identity. */
static int g_drum_held[AZMOD_STEM_DECKS] = {
    0}; /* pad 8 latch: this deck armed; written by the bus thread and the
           overlay (mod off), so every access is __atomic */
/* Pad 7 HOLD per deck (DRUM_HOLD_ON | DRUM_HOLD_SUSTAIN, bus_interpose.c drum_hold_pads).
 * Written by the ErP and mixer bus readers, which run on different threads, so every access
 * is __atomic. */
static int g_drum_hold[AZMOD_STEM_DECKS];
static _Atomic unsigned g_caption_repaint;      /* per-deck bit: caption needs a re-run */
static volatile unsigned g_drum_sel_led[AZMOD_STEM_DECKS] = {0, 0}; /* pads 1-4, for the LED */
/* azstem's exported control surface, resolved by dlsym at overlay init. NULL
 * until azstem.so is loaded; every call site checks. */
static void (*g_drum_gate_fn)(int, int);
static void (*g_drum_select_fn)(int, int);
static void (*g_drum_rate_fn)(int, int);
static void (*g_drum_touch_fn)(int, int);
static void (*g_drum_bank_fn)(int);
static int (*g_drum_avail_fn)(void);
static void (*g_drum_bpm_fn)(int, float); /* per-deck live BPM, see BPMSTATE_CTOR */
static void (*g_drum_sync_fn)(int);       /* BEAT SYNC on/off -> azstem_drum_sync */
static void (*g_drum_tone_fn)(int, int);  /* MIC 2 HI/MID -> azstem_drum_tone (volume, pitch) */
static _Atomic int g_mic2_tone_tlm = -1;  /* telemetry: last (hi << 16) | mid published */
static void (*g_stem_eq_fn)(int, int, int,
                            int); /* channel 3/4 EQ -> azstem_stem_eq (deck, hi, mid, low) */
static int (*g_deck_has_track_fn)(
    int); /* azstem_deck_has_track: deck 3/4 in use -> CH3/4 are deck channels */
static int (*g_stem_gain_fn)(int, int); /* azstem_stem_gain: what Stem Volume plays, x1000 */
static void (*g_sfx_config_fn)(int, int, int); /* azstem_stem_fx_config: Stem FX mode and lengths */
static _Atomic int g_stemeq_knobs[2] = {
    -1, -1}; /* bus thread: (hi << 20) | (mid << 10) | low, -1 = not moved since start */
static _Atomic int g_stemeq_tlm[2] = {
    -1, -1}; /* telemetry: what was last sent per channel, -1 = unity */
/* X-PAD (touch strip) -> roll rate, on the MIXER bus. The emulator does not model
 * the strip, so its bytes are configurable rather than fixed:
 *   /home/root/settings/azmod-xpad
 *     "<pos_byte> [touch_byte] [touch_mask] [full_scale] [zone_byte]"
 * touch_byte < 0 means the position byte is itself the touch tell
 * (non-zero = finger down). */
/* Position byte is 0x1d (walks 0..124). 0x1c is a latched "strip has been
 * used" flag that goes 0->1 once per boot and never clears: reading it pins
 * the roll rate to the lowest zone. */
static int g_xpad_pos_off = XPAD_DEFAULT_POS;
static int g_xpad_touch_off = -1;
static unsigned g_xpad_touch_msk = 0xFF;
/* Full scale of the strip. Measured: 0x1d walks 0..124, so the strip is
 * 7-bit; scaling by 256 would leave the upper three zones unreachable. */
static int g_xpad_span = XPAD_DEFAULT_SPAN;
/* Zone bitmap byte: one bit per x-pad segment (two set mid-transition), in
 * lockstep with the position byte. This is the "finger on strip" signal EP147
 * arms the selected Beat FX from, so the swallow must clear it too. < 0
 * disables. */
static int g_xpad_zone_off = XPAD_DEFAULT_ZONE;
#define XPAD_AUX1_OFF 0x55
#define XPAD_AUX2_OFF 0x57
static _Atomic int g_xpad_raw = -1; /* last raw byte, for INPUT MON */
/* Overlay thread only. The bus reads these through the published settings word
 * (bus_cfg.h), never directly: they are written one at a time. */
static void bus_cfg_publish(void);
static void xpad_load_config(void) {
    FILE *f = fopen("/home/root/settings/azmod-xpad", "re");
    if (!f)
        return;
    int p = XPAD_DEFAULT_POS, t = -1, sp = XPAD_DEFAULT_SPAN, z = XPAD_DEFAULT_ZONE;
    unsigned m = 0xFF;
    if (fscanf(f, "%i %i %i %i %i", &p, &t, (int *) &m, &sp, &z) >= 1) {
        if (p >= 0 && p < 128)
            g_xpad_pos_off = p;
        g_xpad_touch_off = (t >= 0 && t < 128) ? t : -1;
        g_xpad_touch_msk = m ? m : 0xFF;
        if (sp > 1 && sp <= 256)
            g_xpad_span = sp;
        g_xpad_zone_off = (z >= 0 && z < 128) ? z : -1;
    }
    fclose(f);
    bus_cfg_publish(); /* the five fields reach the bus together */
}
/* Is this deck in SLIP LOOP: the drum roll's whole gate. It cannot come from
 * the mode button, because KEY SHIFT is SLIP LOOP's secondary and both set the
 * same bus bit (erp1 byte 0x0d = 0x04). EP147 knows: mode 7 is
 * PadSlipLoopWidget, mode 8 is PadKeyShiftWidget (dispatcher jump table).
 *
 * Read through pad_mode_fresh(), as stems_page() does, because EP147 only
 * republishes the mode on the deck screen: fresh -> EP147's answer;
 * uncommitted -> not slip; stale -> the button seed (exclusive with
 * g_pad_mode_bl). The seed cannot tell KEY SHIFT from SLIP LOOP, so KEY SHIFT
 * entered off the deck screen reads as slip until the waveform is up. */
static _Atomic int g_slip_last[AZMOD_PAD_DECKS]; /* last answer, for while a mode button is held */
static int drum_mode_is_slip(int d, int ld) {
    if (ld < 0 || ld >= AZMOD_STEM_DECKS)
        return 0; /* unknown deck: not ours */
    int fm = pad_mode_fresh(d, ld);
    /* held mode button: EP147 changes mode on release, so keep the last answer */
    if (fm == 0 && __atomic_load_n(&g_mode_down[d], __ATOMIC_RELAXED))
        return __atomic_load_n(&g_slip_last[d], __ATOMIC_RELAXED);
    int r = fm > 0 ? (fm - 1) == dg_slip_enum : fm == 0 ? 0
                                                        : g_pad_mode_slip[d];
    __atomic_store_n(&g_slip_last[d], r, __ATOMIC_RELAXED);
    return r;
}
/* Drop every deck's drum-roll latch. Used by the FX controls below and by the
 * mod-off path, so "armed" can never outlive the thing that armed it. */
static void drum_disarm_all(const char *why) {
    int any = 0;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        if (__atomic_load_n(&g_drum_held[d], __ATOMIC_RELAXED))
            any = 1;
    if (!any)
        return;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        void (*gate)(int, int) = __atomic_load_n(&g_drum_gate_fn, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_drum_held[d], __ATOMIC_RELAXED) && gate)
            gate(d, 0);
        __atomic_store_n(&g_drum_held[d], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_drum_hold[d], 0, __ATOMIC_RELAXED); /* a disarm ends HOLD */
    }
    FILE *f = azlog_open("/tmp/azmod-drumtile.log", "a");
    if (f) {
        fprintf(f, "DISARM ALL (%s)\n", why);
        fclose(f);
    }
}
/* Exclusive arm: drum roll is active on only one deck at a time, so activating
 * it on one deck disables it on every other. Clears every deck's latch and
 * azstem gate except `keep`, so the audio stops with the LEDs. */
static void drum_disarm_others(int keep, const char *why) {
    int did = 0;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        if (d == keep || !__atomic_load_n(&g_drum_held[d], __ATOMIC_RELAXED))
            continue;
        __atomic_store_n(&g_drum_held[d], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_drum_hold[d], 0, __ATOMIC_RELAXED); /* a disarm ends HOLD */
        __atomic_fetch_or(&g_caption_repaint, 1u << d, __ATOMIC_RELEASE);
        void (*gate)(int, int) = __atomic_load_n(&g_drum_gate_fn, __ATOMIC_ACQUIRE);
        if (gate)
            gate(d, 0);
        did = 1;
    }
    if (did) {
        FILE *f = azlog_open("/tmp/azmod-drumtile.log", "a");
        if (f) {
            fprintf(f, "EXCLUSIVE (%s): all decks but %d disarmed\n", why, keep + 1);
            fclose(f);
        }
    }
}

/* ---- Beat FX panel name cell + rate row: native DRUM ROLL takeover ----
 *
 *   - the panel is image art (the Waveform/Fx/beatfx/ images); the DELAY .rodata strings
 *     are debug/HUI only
 *   - name art table 0x3b35728: 28 blobs, 14 blue then 14 white. The name
 *     ImagesComponent is at BeatFxTimeWidget+0x158
 *   - writer 0x1de0148 computes idx = tbl[0x2d64bb0][type] (+14 white when
 *     beat fx is off), stores it at [comp+0xc8] and repaints. An outside field
 *     write is reverted on EP147's next sync, so we hook the writer
 *   - the six X-PAD PARAMETER cells at [BeatFxParamWidget+0x3c8 + 8i] each
 *     hold {bright, dim} juce::Images; EP147 rewrites them on FX change
 *
 * While armed we write idx 28+deck (four appended green "DRUM ROLL[n]" arts)
 * in the writer's shadow and assign roll-rate images {1 .. 1/32} into the
 * cells, re-asserting per tick. Anything EP147 writes while applied is adopted
 * as the saved original, so a select-knob turn while armed still restores the
 * right values. On disarm the writer's own change-only path restores the name.
 *
 * Art is stock from .rodata (fxart.h) except the green names and the 1/32
 * pair, which a build-time generator renders in the panel's own faces. */
#define FX_NAME_UPDATE EP_1de0148  /* BeatFxTimeWidget name-index writer */
#define FX_NAME_COMP 0x158         /* this+0x158 = name ImagesComponent  */
#define FX_PARAM_CELL0 0x3c8       /* BeatFxParamWidget+0x3c8 + 8i = cells */
#define FX_IMG_FROM_MEM EP_23ed7c0 /* juce Image-from-PNG, sret in x8    */
#define FX_IMG_ASSIGN EP_23b1410   /* juce::Image assign (dst*, src*)    */
#define FX_ROLL_NAME_BASE \
    28 /* first appended green art index;                                                          \
         +deck (0..3) = DRUM ROLL[deck+1]   */
/* ---- the X-PAD PARAMETER row: the visible six-value row is one flat
 * ImagesComponent (176x49) owned by a small widget (ctor 0x1de52c0, vt
 * 0x2d72b48) that also holds the active-zone cell (28x31) and the finger
 * indicator (6x41). Its array holds eight native row strips (one per FX-type
 * group); writer 0x1de5120 picks the index from the type table at 0x2d72cb0.
 * The +0x3c8/+0x248 cell comps are image pools, not the drawn surface. */
#define FX_ROW_UPDATE EP_1de5120 /* per-type row-strip index writer   */
#define FX_ROW_UPDATE2 \
    EP_1de51f0                 /* twin writer (identical shape; not                                \
                                    hooking it lets EP147 flip the row                             \
                                    back between our ticks)           */
#define FX_ROW_CTOR EP_1de52c0 /* owner widget ctor (captures this) */
#define FX_ROW_COMP 0x150      /* owner+0x150 = the 176x49 row comp */
#define FX_ROW_STATE 0x138     /* owner+0x138 = FxState*            */
#define FX_ROW_TYPEBYTE 0x51   /* FxState+0x51 = FX type 0..13      */
#define FX_ROW_TBL EP_2d72cb0  /* u32 table: FX type -> strip      */
#define FX_ROW_STRIP_BASE 8    /* first appended strip = idle       */
#define FX_ROW_STRIP_CNT 7     /* idle + zone 0..5                  */
static int g_fxroll_art_ready; /* decoded (the comp growth itself is
                                 re-checked per tick, see below)    */
/* Cached display widgets are wref slots (ep_mem.h): juce::Component's destructor clears them,
 * so a non-zero slot read on the message thread is a live widget. Every one of these classes
 * has juce::Component at offset 0 (RTTI), so each range registers comp_off 0. The arrays some
 * of them live in are declared `_Atomic long`; WSLOT views a slot as the wref type (same size
 * and representation). disp_wref_init (waveform.c) registers every display range once. */
#define WSLOT(p) ((_Atomic uintptr_t *) (p))
static void disp_wref_init(void);
static _Atomic uintptr_t g_fxrow_owner; /* the row widget (XPadParamWidget), captured at ctor */
static _Atomic uintptr_t g_fxparam_w;   /* the BeatFxParamWidget, captured at ctor */
/* The live x-pad zone for the row art is g_fxrow_zone (beat_fx_panel.c):
 * -1 = untouched, else 0..5; written by the bus thread, read on the message
 * thread. */

static int fxname_ensure_grown(long nc); /* defined below, after struct fx_img */

static void (*g_orig_fxname_update)(long);
static void fxname_update_hook(long self) {
    g_orig_fxname_update(self);
    if (!g_fxroll_art_ready)
        return;
    int arm = __atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED);
    /* The arm is exclusive, so the lowest held deck is the armed one. Taking
     * the lowest keeps the index deterministic and, by construction, in 28..31. */
    int deck = -1;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        if (__atomic_load_n(&g_drum_held[d], __ATOMIC_RELAXED)) {
            deck = d;
            break;
        }
    static int last_apply;
    int apply = (arm && deck >= 0);
    if (apply != last_apply) {
        last_apply = apply;
        if (apply)
            dbg("fxname: drum roll DECK %d ARMED -> name cell \"DRUM ROLL[%d]\" (idx %d)\n",
                deck + 1, deck + 1, FX_ROLL_NAME_BASE + deck);
        else
            dbg("fxname: drum roll released -> native name restored by EP147\n");
    }
    if (!apply)
        return;
    int want = FX_ROLL_NAME_BASE + deck;
    uintptr_t v;
    /* self is the writer's own argument, on the message thread: its name comp is live */
    if (!own_q((uintptr_t) self + FX_NAME_COMP, &v) || v <= 0x10000)
        return;
    long comp = (long) v;
    if (!own_q((uintptr_t) comp + 0xc8, &v))
        return;
    if ((uint32_t) v == (uint32_t) want)
        return; /* change-only */
    if (!fxname_ensure_grown(comp))
        return; /* array holds idx? */
    *(volatile uint32_t *) ((uintptr_t) comp + 0xc8) = (uint32_t) want;
    ((void (*)(long)) PAD_REPAINT)(comp);
}

static struct fx_img (*fx_img_from_mem)(const void *data, int size);
static void (*fx_img_assign)(void *dst, const void *src);

/* Each pointer checks itself. A single guard for both lets one caller set
 * fx_img_from_mem alone and leave fx_img_assign NULL, and fxrow_paint then
 * calls through a null pointer. */
static void fx_fns_resolve(void) {
    if (!fx_img_from_mem)
        *(void **) &fx_img_from_mem = (void *) FX_IMG_FROM_MEM;
    if (!fx_img_assign)
        *(void **) &fx_img_assign = (void *) FX_IMG_ASSIGN;
}

static struct fx_img g_fxart_name[AZMOD_STEM_DECKS]; /* decoded DRUM ROLL[1..4]*/
static struct fx_img g_fxart_rate[6][2];             /* [rate][bright/dim]        */
/* [0] = idle strip (all six divisions white); [1+z] = the same row plus a
 * 28x31 gray highlight box (RGB 50,50,50, the native divider color) behind
 * held cell z. */
static struct fx_img g_fxart_strip[FX_ROW_STRIP_CNT];
/* original {bright,dim} of each cell, as of our last apply */
static struct {
    long comp;
    uintptr_t img[2];
    int valid;
} g_fxcap_cell[6];
static int g_fxroll_applied;

static uintptr_t fx_img_val(const struct fx_img *im) {
    return *(const uintptr_t *) im->c;
}
/* Grow the name component's image array 28 -> 28+AZMOD_STEM_DECKS so indexes
 * 28..31 are our DRUM ROLL[deck+1] arts. Keyed on the live count at comp+0xe0,
 * not a once flag: EP147 can rebuild the panel with a fresh 28-entry array, and
 * writing index 28+ into it would read out of bounds at paint. */
static int fxname_ensure_grown(long nc) {
    uintptr_t v; /* nc: the live name comp of the writer's own call */
    if (!own_q((uintptr_t) nc + 0xd0, &v))
        return 0;
    uintptr_t arr = v;
    if (!own_q((uintptr_t) nc + 0xe0, &v))
        return 0;
    uint32_t cnt = (uint32_t) v;
    if (cnt >= FX_ROLL_NAME_BASE + AZMOD_STEM_DECKS)
        return arr > 0x10000;
    if (cnt != 28 || arr <= 0x10000 || !g_fxroll_art_ready)
        return 0;
    enum {
        NEWCNT = 28 + AZMOD_STEM_DECKS
    };
    void *grown = malloc(NEWCNT * 8);
    if (!grown)
        return 0;
    memcpy(grown, (void *) arr, 28 * 8);
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        fx_img_assign((char *) grown + (28 + d) * 8, &g_fxart_name[d]);
    *(volatile uintptr_t *) ((uintptr_t) nc + 0xd0) = (uintptr_t) grown;
    *(volatile uint32_t *) ((uintptr_t) nc + 0xe0) = NEWCNT; /* low 32 bits */
    /* old 224-byte block deliberately leaked: freeing risks racing a
     * paint that still reads it this frame */
    dbg("fxart: name comp %#lx grown to %d imgs (DRUM ROLL[d] = idx %d..%d)\n", (unsigned long) nc,
        NEWCNT, FX_ROLL_NAME_BASE, FX_ROLL_NAME_BASE + AZMOD_STEM_DECKS - 1);
    return 1;
}

/* ---- X-PAD PARAMETER row: same writer's-shadow pattern as the name cell
 * (beat_fx_panel.c). The row comp's array is grown 8 -> 8+FX_ROW_STRIP_CNT,
 * keyed on the live count so a panel rebuilt by EP147 is re-grown. */






static void (*g_orig_fxrow_update2)(long);

static void fxart_load(void) {
    if (g_fxroll_art_ready || !wref_get(&g_fxparam_w))
        return;
    fx_fns_resolve();
    dbg("fxart: decode begin\n");
    {
        static const struct {
            const unsigned char *png;
            unsigned len;
        } NAMES[AZMOD_STEM_DECKS] = {
            {fx_name_drumroll_deck0, FX_NAME_DRUMROLL_DECK0_LEN},
            {fx_name_drumroll_deck1, FX_NAME_DRUMROLL_DECK1_LEN},
            {fx_name_drumroll_deck2, FX_NAME_DRUMROLL_DECK2_LEN},
            {fx_name_drumroll_deck3, FX_NAME_DRUMROLL_DECK3_LEN},
        };
        for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
            g_fxart_name[d] = fx_img_from_mem(NAMES[d].png, (int) NAMES[d].len);
            dbg("fxart: name[%d] decoded bmp=%#lx\n", d + 1,
                (unsigned long) fx_img_val(&g_fxart_name[d]));
        }
    }
    for (int r = 0; r < 6; r++) {
        for (int v = 0; v < 2; v++) {
            const unsigned char *png = 0;
            unsigned len = 0;
            if (r == 5) {
                png = v ? fx_val_1_32_dim : fx_val_1_32_bright;
                len = v ? FX_VAL_1_32_DIM_LEN : FX_VAL_1_32_BRIGHT_LEN;
            } else {
                /* blobs are static arrays; resolve rate -> blob directly */
                if (r == 0) {
                    png = v ? fx_val_1_dim : fx_val_1_bright;
                    len = v ? FX_VAL_1_DIM_LEN : FX_VAL_1_BRIGHT_LEN;
                } else if (r == 1) {
                    png = v ? fx_val_1_2_dim : fx_val_1_2_bright;
                    len = v ? FX_VAL_1_2_DIM_LEN : FX_VAL_1_2_BRIGHT_LEN;
                } else if (r == 2) {
                    png = v ? fx_val_1_4_dim : fx_val_1_4_bright;
                    len = v ? FX_VAL_1_4_DIM_LEN : FX_VAL_1_4_BRIGHT_LEN;
                } else if (r == 3) {
                    png = v ? fx_val_1_8_dim : fx_val_1_8_bright;
                    len = v ? FX_VAL_1_8_DIM_LEN : FX_VAL_1_8_BRIGHT_LEN;
                } else if (r == 4) {
                    png = v ? fx_val_1_16_dim : fx_val_1_16_bright;
                    len = v ? FX_VAL_1_16_DIM_LEN : FX_VAL_1_16_BRIGHT_LEN;
                }
            }
            if (!png) {
                dbg("fxart: missing blob r=%d v=%d\n", r, v);
                return;
            }
            g_fxart_rate[r][v] = fx_img_from_mem(png, (int) len);
        }
    }
    dbg("fxart: 12 rate images decoded\n");
    { /* the seven row strips: idle first, then zones 0..5 */
        static const struct {
            const unsigned char *png;
            unsigned len;
        } STRIPS[FX_ROW_STRIP_CNT] = {
            {fx_strip_idle, FX_STRIP_IDLE_LEN},
            {fx_strip_z0, FX_STRIP_Z0_LEN},
            {fx_strip_z1, FX_STRIP_Z1_LEN},
            {fx_strip_z2, FX_STRIP_Z2_LEN},
            {fx_strip_z3, FX_STRIP_Z3_LEN},
            {fx_strip_z4, FX_STRIP_Z4_LEN},
            {fx_strip_z5, FX_STRIP_Z5_LEN},
        };
        for (int s = 0; s < FX_ROW_STRIP_CNT; s++)
            g_fxart_strip[s] = fx_img_from_mem(STRIPS[s].png, (int) STRIPS[s].len);
    }
    dbg("fxart: 7 row strips decoded\n");
    /* name comp growth is checked per write by fxname_ensure_grown(), since
     * EP147 can rebuild the panel */
    g_fxroll_art_ready = 1;
}

static void fxroll_apply(void) {
    int held = 0;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        if (__atomic_load_n(&g_drum_held[d], __ATOMIC_RELAXED))
            held = 1;
    /* Message thread (rtick). The panel is a wref slot: non-zero means the widget is alive,
     * and its cells are its own fields, read in this call. */
    uintptr_t pw = wref_get(&g_fxparam_w);
    if (!pw)
        return;
    if (!held) {
        if (g_fxroll_applied) {
            /* disarm: put back whatever each cell held when we took it. The captured comp is
             * an identity token only: the cell is re-read from the live panel and restored
             * only while it is still the one we took (a rebuilt panel starts native). */
            for (int i = 0; i < 6; i++) {
                if (!g_fxcap_cell[i].valid)
                    continue;
                uintptr_t v, a = 0;
                if (!own_q(pw + FX_PARAM_CELL0 + i * 8, &v) || (long) v != g_fxcap_cell[i].comp)
                    continue;
                if (!own_q((uintptr_t) g_fxcap_cell[i].comp + 0xd0, &v))
                    continue;
                a = v;
                if (a <= 0x10000)
                    continue;
                fx_img_assign((void *) a, &g_fxcap_cell[i].img[0]);
                fx_img_assign((void *) a + 8, &g_fxcap_cell[i].img[1]);
                ((void (*)(long)) PAD_REPAINT)(g_fxcap_cell[i].comp);
            }
            memset(g_fxcap_cell, 0, sizeof g_fxcap_cell);
            g_fxroll_applied = 0;
            dbg("fxroll: released -> native cells restored\n");
        }
        uintptr_t ow = wref_get(&g_fxrow_owner);
        if (ow)
            fxrow_restore((long) ow);
        return;
    }
    fxart_load();
    if (!g_fxroll_art_ready)
        return;
    fxrow_apply_tick();
    for (int i = 0; i < 6; i++) {
        uintptr_t v, a = 0;
        if (!own_q(pw + FX_PARAM_CELL0 + i * 8, &v) || v <= 0x10000)
            continue;
        long comp = (long) v;
        if (!own_q((uintptr_t) comp + 0xd0, &v) || v <= 0x10000)
            continue;
        a = v;
        uintptr_t cur0 = 0, cur1 = 0;
        own_q(a, &cur0);
        own_q(a + 8, &cur1);
        if (!g_fxroll_applied || !g_fxcap_cell[i].valid) {
            g_fxcap_cell[i].comp = comp;
            g_fxcap_cell[i].img[0] = cur0;
            g_fxcap_cell[i].img[1] = cur1;
            g_fxcap_cell[i].valid = 1;
        } else if (cur0 != fx_img_val(&g_fxart_rate[i][0]) ||
                   cur1 != fx_img_val(&g_fxart_rate[i][1])) {
            /* EP147 rewrote the cell while applied (select knob turned while
             * armed): adopt its images as the new originals, then re-apply. */
            g_fxcap_cell[i].comp = comp;
            g_fxcap_cell[i].img[0] = cur0;
            g_fxcap_cell[i].img[1] = cur1;
        }
        fx_img_assign((void *) a, &g_fxart_rate[i][0]);
        fx_img_assign((void *) a + 8, &g_fxart_rate[i][1]);
        if (!g_fxroll_applied) {
            uintptr_t r0 = 0, r1 = 0;
            own_q(a, &r0);
            own_q(a + 8, &r1);
            dbg("fxroll: cell[%d] comp=%#lx arr=%#lx -> r0=%#lx (want %#lx) r1=%#lx\n", i,
                (unsigned long) comp, (unsigned long) a, (unsigned long) r0,
                fx_img_val(&g_fxart_rate[i][0]), (unsigned long) r1);
        }
        ((void (*)(long)) PAD_REPAINT)(comp);
    }
    if (!g_fxroll_applied)
        dbg("fxroll: armed -> rate cells hold roll rates\n");
    g_fxroll_applied = 1;
}

/* ---- DRUM ROLL bars: VOLUME + PITCH on the REVERB bar ----
 * The REVERB bar is gui::waveform::EffectReverbWidget (vt 0x2d6cac0, ctor
 * 0x1de18a8) at BeatFxParamWidget+0x170:
 *   paint 0x1de1550: drawRect(5,0,166,45,2) in #232323, fillRect(+0xc8 rect
 *   {11,6,154,33}) in color +0xd8 (black), then the same rect cut to
 *   [+0xf0]/100 of its width in color +0xdc (white). +0xf8 is its LENGTH
 *   readout child.
 * Layout switch 0x1dde9e8(this, type, sub) sets each child's visibility per
 * FX type; type 5 (REVERB) shows only +0x170. While drum roll holds the
 * panel the layout runs as type 5, LENGTH is hidden, and the bar paints as
 * two bars under VOLUME / PITCH labels (fxbars.h). EP147 re-runs the layout on
 * every type change and rebuild, so the beat row cannot return mid-roll. */
#include "fxbars.h"
static void mp_image_from_memory(uintptr_t *out, const void *data, int size); /* menu_popup.c */
#define FXB_LAYOUT EP_1dde9e8   /* BeatFxParamWidget layout(this, type, sub) */
#define FXB_PAINT EP_1de1550    /* EffectReverbWidget::paint(this, Graphics&) */
#define FXB_PARAM_VT EP_2d6c498 /* BeatFxParamWidget vtable (ctor 0x1de01d8) */
#define FXB_REVERB_VT EP_2d6cac0
#define FXB_PARAM_REVERB 0x170
#define FXB_PARAM_STATE 0xf8     /* FxState*: +0x51 type, +0x58 sub (0x1ddff78) */
#define FXB_REVERB_LEN 0xf8      /* the bar widget's LENGTH readout */
#define FXB_SETCOLOUR EP_23af140 /* (Graphics*, Colour*); Colour = u32 ARGB */
#define FXB_DRAWRECT EP_23afeb0  /* (Graphics*, x, y, w, h, thickness) */
#define FXB_FILLRECTF EP_23af6c0 /* (Graphics*, float x, y, w, h) */
#define FXB_DRAWIMAGE EP_23de290
static int g_fxbars_deck = -1; /* deck the bars show; -1 = stock panel (message thread) */
static void (*g_orig_fxb_layout)(long, unsigned, unsigned);
static void (*g_orig_fxb_paint)(long, void *);
static uintptr_t g_fxb_img[2];

/* obj must be live (a hook argument, a field of one, or a non-zero wref slot on the message
 * thread): this proves its class, not that it exists. */
static int fxb_vt_is(uintptr_t obj, uintptr_t vt) {
    uintptr_t v;
    return obj > 0x10000 && (obj & 7) == 0 && own_q(obj, &v) && v == vt;
}
static void fxb_set_visible(uintptr_t comp, int on) {
    uintptr_t vt, fn;
    if (comp <= 0x10000 || (comp & 7) || !own_q(comp, &vt) || !img_q(vt + 0x60, &fn))
        return;
    ((void (*)(uintptr_t, int)) fn)(comp, on);
}
static void fxb_layout_hook(long self, unsigned type, unsigned sub) {
    int on = g_fxbars_deck >= 0;
    g_orig_fxb_layout(self, on ? 5u : type, sub);
    uintptr_t rv, len;
    if (own_q((uintptr_t) self + FXB_PARAM_REVERB, &rv) && fxb_vt_is(rv, FXB_REVERB_VT) &&
        own_q(rv + FXB_REVERB_LEN, &len)) /* self: the layout's own argument */
        fxb_set_visible(len, !on); /* stock: the ctor leaves it visible */
}
static void fxb_color(void *g, uint32_t argb) {
    ((void (*)(void *, uint32_t *)) FXB_SETCOLOUR)(g, &argb);
}
static void fxb_fill(void *g, float x, float y, float w, float h) {
    ((void (*)(void *, float, float, float, float)) FXB_FILLRECTF)(g, x, y, w, h);
}
/* one bar: native frame + well at native x/width, 34 tall, so two bars and
 * their labels fit the 102 px of the native bar + LENGTH row */
static void fxb_box(void *g, int y, uint32_t well) {
    fxb_color(g, 0xff232323);
    ((void (*)(void *, int, int, int, int, int)) FXB_DRAWRECT)(g, 5, y, 166, 34, 2);
    fxb_color(g, well);
    fxb_fill(g, 11, y + 6, 154, 22);
}
static void fxb_label(void *g, int which, int y) {
    if (!g_fxb_img[which]) {
        uintptr_t img = 0, q = 0;
        mp_image_from_memory(&img, which ? FXB_LBL_PITCH : FXB_LBL_VOLUME,
                             which ? (int) sizeof FXB_LBL_PITCH : (int) sizeof FXB_LBL_VOLUME);
        if (!img || !own_q(img + 0x10, &q) || (int) (uint32_t) q != FXB_LBL_W || /* our image */
            (int) (uint32_t) (q >> 32) != FXB_LBL_H)
            return;
        g_fxb_img[which] = img;
    }
    uintptr_t holder = g_fxb_img[which];
    ((void (*)(void *, uintptr_t *, int, int, int, int, int, int, int, int, int)) FXB_DRAWIMAGE)(
        g, &holder, 5, y, FXB_LBL_W, FXB_LBL_H, 0, 0, FXB_LBL_W, FXB_LBL_H, 0);
}
/* one MIC 2 knob (10-bit, 12 o'clock = 512) as a bar filled out from a center
 * mark. The dead band matches azstem_drum_tone's (506..518), so the bar reads
 * "unchanged" exactly when the sound is. */
static void fxb_center_bar(void *g, int y, int v10, uint32_t well, uint32_t ink) {
    float off = (v10 >= 506 && v10 <= 518) ? 0.f : (float) (v10 - 512) / 511.f * 77.f;
    fxb_box(g, y, well);
    fxb_color(g, ink);
    if (off > 0)
        fxb_fill(g, 88, y + 6, off, 22);
    else if (off < 0)
        fxb_fill(g, 88 + off, y + 6, -off, 22);
    fxb_color(g, 0xff969696); /* the labels' gray */
    fxb_fill(g, 87, y + 6, 2, 22);
}
static void fxb_paint_hook(long self, void *g) {
    int deck = g_fxbars_deck;
    if (deck < 0 || deck >= AZMOD_STEM_DECKS) {
        g_orig_fxb_paint(self, g);
        return;
    }
    uint32_t well = *(uint32_t *) (self + 0xd8), ink = *(uint32_t *) (self + 0xdc);
    /* VOLUME = MIC 2 HI, PITCH = MIC 2 MID (azstem_drum_tone) */
    int t = __atomic_load_n(&g_mic2_tone_tlm, __ATOMIC_RELAXED);
    int hi = t < 0 ? 512 : (t >> 16) & 0xffff, mid = t < 0 ? 512 : (t & 0xffff);
    fxb_label(g, 0, 0);
    fxb_center_bar(g, 13, hi, well, ink);
    fxb_label(g, 1, 51);
    fxb_center_bar(g, 64, mid, well, ink);
}

/* ---- DRUM ROLL: the stock blue finger line on the X-PAD row, for every FX ----
 * The row widget (vt 0x2d72b48) has a 6x41 finger cell at +0x158 and a 28x31
 * zone cell at +0x148. Dispatcher 0x1de50e0 picks by FX type
 * ([[owner+0x138]+0x51]): 0-2, 10-12 -> zone mode 0x1de4fb8 (zone cell at
 * [+0x5f], finger hidden); 3-9, 13 -> finger mode 0x1de4ee0 (finger at
 * FxState[+0x5e] within [+0x128..+0x12c], hidden outside = not touched). The
 * position arrives for every type, so while drum roll holds the panel zone
 * mode runs finger mode instead; our highlight strip (+0x150) is untouched.
 * Arm/disarm re-runs the dispatcher so the switch is immediate. */
#define FXR_DISPATCH EP_1de50e0
#define FXR_ZONE EP_1de4fb8
#define FXR_FINGER EP_1de4ee0
#define FXR_OWNER_VT EP_2d72b48
static void (*g_orig_fxr_zone)(long);
static void fxr_zone_hook(long owner) {
    if (g_fxbars_deck >= 0) {
        ((void (*)(long)) FXR_FINGER)(owner);
        return;
    }
    g_orig_fxr_zone(owner);
}
/* message thread: re-run EP147's own mode choice for the row after arm/disarm */
static void fxr_redispatch(void) {
    uintptr_t ow = wref_get(&g_fxrow_owner);
    if (fxb_vt_is(ow, FXR_OWNER_VT))
        ((void (*)(long)) FXR_DISPATCH)((long) ow);
}

/* ---- DRUM ROLL: the X-PAD PARAMETER header bar says ROLL RATE (green) ----
 * The bar is an ImagesComponent at parent+0xe0 of the X-PAD block (parent vt
 * 0x2d74bb0, ctor 0x1de5ea0, art img_xpad_name_* from 0x3b35ad8 on). Writer
 * 0x1de5e30(parent, type) sets [comp+0xc8] = tbl[0x2d74d10][type] and repaints,
 * the name cell's pattern. While drum roll holds the panel the hook points it
 * at one appended image (fxhdr.h). The array counts as grown when its last
 * entry is already our image (a juce::Image is its pixel-data pointer), so a
 * rebuilt panel is re-grown. Arm/disarm re-runs the writer with the real type. */
#include "fxhdr.h"
#define FXH_WRITER EP_1de5e30
#define FXH_PARENT_VT EP_2d74bb0
#define FXH_COMP 0xe0
static void (*g_orig_fxh_writer)(long, unsigned);
static _Atomic uintptr_t g_fxh_parent; /* the X-PAD block (XPadWidget), from its ctor: a wref */
/* Captured from the ctor: the writer runs only on an FX type change, so
 * waiting for it would leave the arm-time refresh with no parent. */
#define FXH_CTOR EP_1de5ea0
static void (*g_orig_fxh_ctor)(long, long);
static void fxh_ctor_hook(long self, long a1) {
    g_orig_fxh_ctor(self, a1);
    disp_wref_init();
    wref_set(&g_fxh_parent, (uintptr_t) self);
}
static uintptr_t g_fxh_img, g_fxf_img; /* decoded ROLL RATE bar / lime finger line */
/* index of an appended image in an ImagesComponent's array, appending it once;
 * -1 = cannot. The array counts as grown when its last entry is already ours. */
static int fx_appended_index(uintptr_t comp, uintptr_t *cache, const unsigned char *png, int size,
                             int w, int h) {
    uintptr_t arr, cnt, last = 0;
    if (!*cache) {
        uintptr_t img = 0, q = 0;
        mp_image_from_memory(&img, png, size);
        if (!img || !own_q(img + 0x10, &q) || (int) (uint32_t) q != w || /* our image */
            (int) (uint32_t) (q >> 32) != h)
            return -1;
        *cache = img;
    }
    uintptr_t want = *cache; /* comp: a live ImagesComponent the caller proved by vtable */
    if (!own_q(comp + 0xd0, &arr) || arr <= 0x10000 || !own_q(comp + 0xe0, &cnt))
        return -1;
    uint32_t n = (uint32_t) cnt;
    if (n == 0 || n > 64)
        return -1;
    if (own_q(arr + (n - 1) * 8, &last) && last == want)
        return (int) n - 1;
    void *grown = malloc((n + 1) * 8);
    if (!grown)
        return -1;
    memcpy(grown, (void *) arr, n * 8);
    fx_fns_resolve();
    *(uintptr_t *) ((char *) grown + n * 8) = 0;
    fx_img_assign((char *) grown + n * 8, cache);
    *(volatile uintptr_t *) (comp + 0xd0) = (uintptr_t) grown;
    *(volatile uint32_t *) (comp + 0xe0) = n + 1;
    /* old block leaked on purpose: a paint may still read it this frame */
    return (int) n;
}
/* while drum roll holds the panel, point the header at ROLL RATE */
static void fxh_apply(long parent) {
    if (g_fxbars_deck < 0)
        return;
    uintptr_t comp;
    if (!own_q((uintptr_t) parent + FXH_COMP, &comp) || !fxb_vt_is(comp, IMAGESCOMP_VT))
        return;
    int i =
        fx_appended_index(comp, &g_fxh_img, FXH_ROLLRATE, (int) sizeof FXH_ROLLRATE, FXH_W, FXH_H);
    if (i < 0)
        return;
    uintptr_t cur;
    if (own_q(comp + 0xc8, &cur) && (uint32_t) cur == (uint32_t) i)
        return;
    *(volatile uint32_t *) (comp + 0xc8) = (uint32_t) i;
    ((void (*)(long)) PAD_REPAINT)((long) comp);
}
/* writer 1: 0x1de5e30(parent, type), reached from 0x1de5e20 on an FX type change */
static void fxh_writer_hook(long parent, unsigned type) {
    g_orig_fxh_writer(parent, type);
    fxh_apply(parent);
}
/* writer 2, the twin: 0x1de5d90(parent), called per panel update from
 * 0x1de7b04 (it also drives the row's twin writer 0x1de51f0). It re-sets the
 * header index from the FX type, so unhooked it reverts ours within ~0.3 s. */
#define FXH_WRITER2 EP_1de5d90
static void (*g_orig_fxh_writer2)(long);
static void fxh_writer2_hook(long parent) {
    g_orig_fxh_writer2(parent);
    fxh_apply(parent);
}
/* The finger line turns lime while drum roll is armed. The finger cell (row
 * owner+0x158, holding img_xpad_bar.png) gets one appended lime copy
 * (fxhdr.h); armed = that index, re-asserted per tick since a rebuilt row
 * starts native; disarmed = the saved index. Message thread only. */
static void fxf_tick(int armed) {
    static uintptr_t saved_comp;
    static uint32_t saved_idx;
    uintptr_t ow = wref_get(&g_fxrow_owner), fc, cur;
    if (!fxb_vt_is(ow, FXR_OWNER_VT) || !own_q(ow + 0x158, &fc) || !fxb_vt_is(fc, IMAGESCOMP_VT) ||
        !own_q(fc + 0xc8, &cur)) {
        saved_comp = 0;
        return;
    }
    if (armed) {
        int i =
            fx_appended_index(fc, &g_fxf_img, FXH_FINGER, (int) sizeof FXH_FINGER, FXF_W, FXF_H);
        if (i < 0 || (uint32_t) cur == (uint32_t) i)
            return;
        if (saved_comp != fc) {
            saved_comp = fc;
            saved_idx = (uint32_t) cur;
        }
        *(volatile uint32_t *) (fc + 0xc8) = (uint32_t) i;
        ((void (*)(long)) PAD_REPAINT)((long) fc);
    } else if (saved_comp == fc) {
        *(volatile uint32_t *) (fc + 0xc8) = saved_idx;
        ((void (*)(long)) PAD_REPAINT)((long) fc);
        saved_comp = 0;
    }
}
/* message thread, on arm/disarm: re-run the writer with EP147's own type */
static void fxh_refresh(unsigned type) {
    uintptr_t par = wref_get(&g_fxh_parent);
    if (fxb_vt_is(par, FXH_PARENT_VT))
        ((void (*)(long, unsigned)) FXH_WRITER)((long) par, type);
}
/* message thread (repaint timer): relayout on arm/disarm, repaint on change */
static void fxbars_tick(void) {
    static int last_deck = -1, last_sig = -1;
    int deck = -1;
    if (__atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED))
        for (int d = 0; d < AZMOD_STEM_DECKS; d++)
            if (__atomic_load_n(&g_drum_held[d], __ATOMIC_RELAXED)) {
                deck = d;
                break;
            }
    g_fxbars_deck = deck;
    uintptr_t pw = wref_get(&g_fxparam_w), st, rv, q50 = 0, q58 = 0; /* 0 once destroyed */
    if (!fxb_vt_is(pw, FXB_PARAM_VT))
        return; /* no live panel: nothing to lay out */
    if (deck != last_deck) {
        if (!own_q(pw + FXB_PARAM_STATE, &st) || st <= 0x10000 || !own_q(st + 0x50, &q50) ||
            !own_q(st + 0x58, &q58))
            return;
        /* through the hook: it substitutes type 5 while armed; on release
         * EP147's own type puts the stock layout back */
        ((void (*)(long, unsigned, unsigned)) FXB_LAYOUT)((long) pw, (unsigned) (q50 >> 8) & 0xff,
                                                          (unsigned) q58 & 0xff);
        dbg("fxbars: deck %d -> %s\n", deck + 1, deck >= 0 ? "VOLUME/PITCH bars" : "stock layout");
        fxr_redispatch(); /* the X-PAD row: finger line on / stock mode back */
        fxh_refresh((unsigned) (q50 >> 8) & 0xff); /* the X-PAD header: ROLL RATE / stock */
        last_deck = deck;
        last_sig = -1;
    }
    fxf_tick(deck >= 0); /* the finger line: lime while armed */
    if (deck < 0)
        return;
    int sig = __atomic_load_n(&g_mic2_tone_tlm, __ATOMIC_RELAXED); /* (HI << 16) | MID */
    if (sig == last_sig)
        return;
    last_sig = sig;
    if (own_q(pw + FXB_PARAM_REVERB, &rv) && fxb_vt_is(rv, FXB_REVERB_VT))
        ((void (*)(long)) PAD_REPAINT)((long) rv);
}
/* ---- DRUM ROLL tile: the 8th SLIP LOOP cell --------------------------------
 * The pad art is a table of {down,normal} pointer pairs in writable .data.
 * Tile 8's pair sits four words below PAD_ASSET_TBL:
 *     0x3b36028 -> 0x2d8da20   (down   / pngpatched green "DRUM ROLL")
 *     0x3b36030 -> 0x2d8d7a0   (normal / pngpatched red   "DRUM ROLL")
 * 0x2d8d7a0 is the second, unmapped copy of img_pad_3_4_normal and is the one
 * the SLIP LOOP grid renders; the mapped copy at 0x2d824e0 is not drawn.
 * Lit state is driven by the art index below, not by swapping these pointers. */
#define DRUM_TILE_TBL 0x3b36028UL /* [0]=down(green) [1]=normal(red) */
#define DRUM_TILE_DOWN EP_2d8da20
#define DRUM_TILE_NORM EP_2d8d7a0
static _Atomic int g_drum_tile_repaint; /* set by drum_tile_apply, drained on the JUCE tick */

/* ---- DRUM ROLL tile-8 index on the stems grid ------------------------------
 * The armed SLIP LOOP grid is the forced stems grid, whose slot-7 art is the
 * pngpatched drums_echo pair: {down=0x18 green, normal=0x19 red} DRUM ROLL.
 * The latch switches the index in the component ([stem+0xf0]+0xc8). pngpatch
 * runs synchronously in the constructor, so the decoded arts are fixed. */
#define DGRID_GRIDS 4
#define DGRID_SLOTS 8
static _Atomic int g_drum_tile_repaint;      /* raised by the index apply, drained on rtick */
static _Atomic long g_dg8_stem[DGRID_GRIDS]; /* slot-7 PadStemWidget per grid */
static unsigned g_dg8_idx_refused = 0;       /* out-of-range art-index writes refused */
static int g_dg8_idx_logged = -1;
/* stems_display.c: per-deck requests to re-run the stem tiles' refresh (message thread drains) */
static _Atomic unsigned g_tiles_poke_req;
/* Message thread (rtick), the thread that builds and frees these widgets. Nothing cached is
 * trusted: each pass starts from the slot-7 tile captured in its ctor (g_slot7_w), proves it by
 * vtable, and re-derives the stem child and its ImagesComponent from it, each proven by vtable,
 * before the one write. g_dg8_stem (from the same ctor) must still match the re-derived child:
 * a tile rebuilt since then is skipped until its ctor recaptures it. The deck comes from the
 * tile's own deckstate (+0xd0); an unknown deck leaves the tile alone.
 * The tile belongs to the drum roll only while it is latched: unlatched, the same slot is the
 * stems page's DRUMS ECHO tile, whose index EP147 and Stem FX (stem_tile_fix_one) set. So the
 * normal index is written once, on the release of a latch this code showed, and the stem tiles
 * of that deck are then refreshed so an effect that is on shows again. */
static void drum8_index_apply(void) {
    static unsigned char shown[DGRID_GRIDS]; /* this grid's tile shows the latch (0x18) */
    int changed = 0;
    for (int g = 0; g < DGRID_GRIDS && g < SLOT7_MAX; g++) {
        long w = (long) wref_get(WSLOT(&g_slot7_w[g])); /* wref: 0 once the tile is destroyed */
        long cap = __atomic_load_n(&g_dg8_stem[g], __ATOMIC_RELAXED); /* identity token only */
        if (w <= 0x10000 || (w & 7) || cap <= 0x10000)
            continue;
        uintptr_t a1[3] = {(uintptr_t) w, (uintptr_t) w + 0x110, (uintptr_t) w + 0xd0}, v1[3];
        int ok1[3];
        own_qv(3, a1, v1, ok1);
        if (!ok1[0] || (long) v1[0] != (long) PADTILE_VT || !ok1[1] || (long) v1[1] != cap ||
            !ok1[2])
            continue; /* the tile is gone or rebuilt: wait for its ctor */
        long stem = (long) v1[1];
        int gdeck = deck_for_modeobj((long) v1[2]);
        if (gdeck < 0 || gdeck >= AZMOD_STEM_DECKS)
            continue; /* unknown deck: never another deck's latch */
        int armed = __atomic_load_n(&g_drum_held[gdeck], __ATOMIC_RELAXED);
        int want = (__atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED) && armed) ? 0x18 : 0x19;
        if (want == 0x19 && !shown[g])
            continue; /* not latched: the stems page's tile, not ours */
        uintptr_t comp_v;
        if ((stem & 7) || !own_q((uintptr_t) stem + 0xf0, &comp_v)) /* the live tile's child */
            continue;
        long comp = (long) comp_v;
        uintptr_t a2[3] = {(uintptr_t) comp, (uintptr_t) comp + 0xe0, (uintptr_t) comp + 0xc8}, v2[3];
        int ok2[3];
        own_qv(3, a2, v2, ok2);
        if (!ok2[0] || (long) v2[0] != (long) IMAGESCOMP_VT || !ok2[1] || !ok2[2])
            continue;
        /* The array must hold the index: [comp+0xe0] is the image count. Out
         * of range, EP147 paints through a juce::Image read off the end of
         * the array (a SIGSEGV in its rectangle fill). The counter records
         * refusals. */
        uint32_t cnt = (uint32_t) v2[1];
        if (cnt <= (uint32_t) want) {
            unsigned n = __atomic_add_fetch(&g_dg8_idx_refused, 1, __ATOMIC_RELAXED);
            if (n == 1 || (n % 256) == 0) {
                FILE *f = azlog_open("/tmp/azmod-drumtile.log", "a");
                if (f) {
                    fprintf(f, "TILE8 REFUSED g=%d comp=%#lx cnt=%u want=%d (n=%u)\n", g,
                            (unsigned long) comp, cnt, want, n);
                    fclose(f);
                }
            }
            continue;
        }
        shown[g] = want == 0x18;
        if (!shown[g]) /* released: Stem FX shows this deck's tiles again */
            __atomic_fetch_or(&g_tiles_poke_req, 1u << gdeck, __ATOMIC_RELEASE);
        if ((int) (uint32_t) v2[2] == want)
            continue;
        *(volatile int *) (comp + 0xc8) = want; /* comp proven an ImagesComponent just above */
        changed++;
        if (g_dg8_idx_logged != want) {
            FILE *f = azlog_open("/tmp/azmod-drumtile.log", "a");
            if (f) {
                fprintf(f, "TILE8 g=%d deck=%d idx=%s\n", g, gdeck, want == 0x18 ? "green" : "red");
                fclose(f);
            }
            g_dg8_idx_logged = want;
        }
    }
    if (changed)
        __atomic_store_n(&g_drum_tile_repaint, 1, __ATOMIC_RELEASE);
}
/* the latch/grid repaint drain: drum8_index_apply raises the flag when the
 * tile-8 index flips; slot7_repaint_all repaints the four grid instances */
static void drum_tile_repaint_if_needed(void) {
    drum8_index_apply();
    if (!__atomic_exchange_n(&g_drum_tile_repaint, 0, __ATOMIC_ACQ_REL))
        return;
    slot7_repaint_all();
}






/* ---- DRUM ROLL: persistent selected sample / armed pad-8 tile art ---------
 * The selected pad of 1-4 shows the green highlight; pad 8 is red when off,
 * green when on.
 *
 * Refresh FUN_01df69f8 writes the art index `2*[w+0xc8] + 1` (the normal entry
 * of that slot's pair) into the model at `[w+0xf0]+0xc8`, so `2*slot` is the
 * down entry and lighting a tile is one integer store.
 *
 * PadSlipLoopWidget (container+0x100) is laid out like PadStemWidget: a
 * PadStatusWidget back-pointer at +0x18 and an ImagesComponent at +0xf0. The
 * tile itself and [tile+0xf0] are PadCueWidget-based, the wrong objects.
 *
 * Default on; azmod-drumsel.off disables it. */
/* SLIP LOOP art block: nine {down,normal} pairs at 0x3b35fb8, ending where the
 * stems array begins at 0x3b36048 (index 18: a ninth pair sits between tile 8
 * and stems). Anchors are down entries plus the first stems entry, values this
 * code never writes. */
#define SLIP_TBL EP_3b35fb8
#define SLIP_A0_DOWN EP_2d8f570 /* pair 0 down = img_pad_1_16_down */
#define SLIP_A1_DOWN 0x2d8f090L /* pair 1 down = img_pad_1_8_down  */
#define SLIP_A7_DOWN EP_2d8da20 /* pair 7 down = green DRUM ROLL   */
#define SLIP_STEMS0 EP_2d95250  /* img_pad_vocal_down, straight after */
#define SLIP_CHILD_OFF 0x100UL
#define SLIP_REFRESH \
    EP_1df57c8                /* PadSlipLoopWidget refresh, from WAVE_SWITCH's                     \
                               * call sites (bl order is not offset order:                         \
                               * guessing by position finds PadCueWidget) */
#define TILE_MODEL_OFF 0xf0UL /* -> ImagesComponent */
#define MODEL_IDX_OFF 0xc8UL  /* the art index inside it */
#define TILE_SLOT_OFF 0xc8UL  /* slot 0..7, on the child too */


/* DRUM ROLL tile state: the art index is re-asserted from the UI thread on
 * every paint. EP147's refresh 0x1df57c8 recomputes it (stores at +0x48 and
 * +0x78, then repaints), so a one-off write is overwritten. The refresh cannot
 * be trampoline-hooked: a cbnz at +0x08 is PC-relative inside the prologue
 * azm_install_hook relocates, so the install refuses. A per-paint store fixes
 * any revert within a frame at the cost of one integer store per tile.
 *
 * Index convention: `2*slot + 1` is the normal entry, `2*slot` the down entry.
 * Our art lives in the even entries; pngpatch leaves the odd ones native, so
 * with nothing written the grid displays as stock. Index changes only, never a
 * table swap, and nothing here touches the stems block (0x3b36048+).
 *
 * Index map, mirroring the tile-art generator's entry table:
 *     tile 1-4 unselected -> 0, 2, 4, 6
 *     tile 1-4 selected   -> 10, 12, 8, 16
 *     tile 8 armed        -> 14  (DRUM ROLL, green)
 *     tile 8 default      -> 15  (2*7+1, red DRUM ROLL: disarmed needs no write)
 *     tiles 5-6 armed     -> 17, the blank
 *     tile 7 armed        -> 18 (HOLD) / 19 (HOLD on), appended by hold_tile_ready
 * With the Drum Roll mod off, entries 14/15 are swapped back to the native
 * 3/4 art (slip34_apply). */
static const int DRUM_IDX_PLAIN[4] = {0, 2, 4, 6};
static const int DRUM_IDX_LIT[4] = {10, 12, 8, 16};
#define DRUM_IDX_TILE8 14
#define DRUM_IDX_BLANK 17 /* the one spare entry in the tile art */
#define DRUM_IDX_HOLD 18  /* tile 7's appended HOLD art; HOLD on is the next entry */

/* ---- Tile 8 with Drum Roll off: EP147's own 3/4 ----
 * With the mod off, pad 8 is the native 3/4 slip loop, so its tile must not
 * read DRUM ROLL. pngpatch overwrote both 3/4 arts the grid decodes (normal
 * 0x2d8d7a0, down 0x2d8da20), but the normal art has an untouched,
 * byte-identical copy at 0x2d824e0; it serves for both states.
 *
 * Tile 8's array is grown 18 -> 20 once, parking the red/green DRUM ROLL images
 * at 18/19. Mod off -> entries 14 and 15 hold the native 3/4, so whatever index
 * EP147 writes (15 resting, 14 held) lands on native art. Mod on -> 14/15
 * restored from 19/18. Keyed on the live count, so a rebuilt array is
 * re-grown. The source bytes are fingerprinted; anything but the stock PNG is
 * refused and the tile is left alone. */
#define SLIP34_SRC EP_2d824e0 /* img_pad_3_4_normal, the untouched copy */
#define SLIP34_LEN 622
#define SLIP34_FNV 0x074d304c23367db7ULL /* FNV-1a 64 of the stock bytes */
#define SLIP_ART_CNT 18
static struct fx_img g_slip34_art;
static int g_slip34_state; /* 0 untried, 1 decoded, -1 refused */
static int slip34_ready(void) {
    if (g_slip34_state)
        return g_slip34_state > 0;
    const unsigned char *p = (const unsigned char *) SLIP34_SRC;
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < SLIP34_LEN; i++)
        h = (h ^ p[i]) * 0x100000001b3ULL;
    if (h != SLIP34_FNV) {
        g_slip34_state = -1;
        dbg("slip34: native 3/4 art at %#lx is not stock (fnv %#llx) -- tile 8 left alone\n",
            SLIP34_SRC, h);
        return 0;
    }
    fx_fns_resolve();
    g_slip34_art = fx_img_from_mem(p, SLIP34_LEN);
    if (!fx_img_val(&g_slip34_art))
        return 0; /* decoder not ready: retry next tick */
    g_slip34_state = 1;
    dbg("slip34: native 3/4 decoded for tile 8 (Drum Roll off)\n");
    return 1;
}
/* comp = tile 8's ImagesComponent (vtable-checked by the caller); UI thread. */
static void slip34_apply(long comp, long tw, int native) {
    if (comp <= 0x10000 || (comp & 7) || *(long *) comp != (long) IMAGESCOMP_VT)
        return;
    uintptr_t *arrp = (uintptr_t *) (comp + 0xd0);
    uint32_t *cntp = (uint32_t *) (comp + 0xe0);
    uintptr_t arr = *arrp;
    uint32_t cnt = *cntp;
    if (arr <= 0x10000 || (arr & 7))
        return;
    if (cnt == SLIP_ART_CNT) {
        if (!native || !slip34_ready())
            return; /* never grown: nothing to undo */
        void *grown = malloc((SLIP_ART_CNT + 2) * 8);
        if (!grown)
            return;
        memcpy(grown, (void *) arr, SLIP_ART_CNT * 8);
        fx_img_assign((char *) grown + 18 * 8, (void *) (arr + 15 * 8)); /* red   DRUM ROLL */
        fx_img_assign((char *) grown + 19 * 8, (void *) (arr + 14 * 8)); /* green DRUM ROLL */
        *(volatile uintptr_t *) arrp = (uintptr_t) grown;
        *(volatile uint32_t *) cntp = SLIP_ART_CNT + 2;
        /* old block leaked on purpose: a paint may still read it this frame */
        arr = (uintptr_t) grown;
        cnt = SLIP_ART_CNT + 2;
    }
    if (cnt != SLIP_ART_CNT + 2)
        return; /* not an array we grew */
    uintptr_t *a = (uintptr_t *) arr;
    if (native) {
        if (g_slip34_state <= 0)
            return;
        uintptr_t nat = fx_img_val(&g_slip34_art);
        if (a[14] == nat && a[15] == nat)
            return;
        fx_img_assign(&a[14], &g_slip34_art);
        fx_img_assign(&a[15], &g_slip34_art);
    } else {
        if (a[14] == a[19] && a[15] == a[18])
            return;
        fx_img_assign(&a[14], &a[19]);
        fx_img_assign(&a[15], &a[18]);
    }
    ((void (*)(long)) PAD_REPAINT)(tw);
}

/* ---- Tile 7 while armed: HOLD ----
 * EP147 has no HOLD pad art, so tile 7's array is grown 18 -> 20 with the two
 * holdtile.h images at 18 (HOLD) and 19 (HOLD on). Keyed on the live count, so
 * a rebuilt array is re-grown. Returns 1 only when entries 18/19 are ours; any
 * other array is left alone and the caller shows the blank instead.
 * model = [tile 7's child + 0xf0], the child proven by the caller; UI thread. */
#include "holdtile.h"
static struct fx_img g_hold_art[2];
static int g_hold_art_ok;
static int hold_tile_ready(long model) {
    if (!g_hold_art_ok) {
        fx_fns_resolve();
        if (!fx_img_val(&g_hold_art[0]))
            g_hold_art[0] = fx_img_from_mem(HOLD_TILE_OFF, (int) sizeof HOLD_TILE_OFF);
        if (!fx_img_val(&g_hold_art[1]))
            g_hold_art[1] = fx_img_from_mem(HOLD_TILE_ON, (int) sizeof HOLD_TILE_ON);
        if (!fx_img_val(&g_hold_art[0]) || !fx_img_val(&g_hold_art[1]))
            return 0; /* decoder not ready: retry next paint */
        g_hold_art_ok = 1;
    }
    if (model <= 0x10000 || (model & 7) || *(long *) model != (long) IMAGESCOMP_VT)
        return 0;
    uintptr_t *arrp = (uintptr_t *) (model + 0xd0);
    uint32_t *cntp = (uint32_t *) (model + 0xe0);
    uintptr_t arr = *arrp;
    if (arr <= 0x10000 || (arr & 7))
        return 0;
    if (*cntp == SLIP_ART_CNT) {
        void *grown = malloc((SLIP_ART_CNT + 2) * 8);
        if (!grown)
            return 0;
        memcpy(grown, (void *) arr, SLIP_ART_CNT * 8);
        memset((char *) grown + SLIP_ART_CNT * 8, 0, 2 * 8);
        fx_img_assign((char *) grown + DRUM_IDX_HOLD * 8, &g_hold_art[0]);
        fx_img_assign((char *) grown + (DRUM_IDX_HOLD + 1) * 8, &g_hold_art[1]);
        *(volatile uintptr_t *) arrp = (uintptr_t) grown;
        *(volatile uint32_t *) cntp = SLIP_ART_CNT + 2;
        /* old block leaked on purpose: a paint may still read it this frame */
        arr = (uintptr_t) grown;
    }
    if (*cntp != SLIP_ART_CNT + 2)
        return 0;
    const uintptr_t *a = (const uintptr_t *) arr;
    return a[DRUM_IDX_HOLD] == fx_img_val(&g_hold_art[0]) &&
           a[DRUM_IDX_HOLD + 1] == fx_img_val(&g_hold_art[1]);
}

static void drum_tiles_enforce(void) {
    /* Default on; azmod-drumsel.off opts out. Re-read every 256 calls to keep
     * access() off the per-paint path. */
    {
        static int off = -1;
        static unsigned n;
        if (off < 0 || (++n & 0xffu) == 0)
            off = (access("/home/root/settings/azmod-drumsel.off", F_OK) == 0);
        if (off)
            return;
    }
    /* Must run every paint: EP147 rewrites the tile index on every paint, so
     * throttling this breaks the tiles.
     * Step 0 decides per (grid, slot) what index a tile should carry; steps
     * 1-2 read each live tile (a non-zero wref slot, on the message thread) and
     * its child, proving their classes; step 3 applies. */
    struct {
        long tw;
        int want;
        int disarmed;
        int hold;
        int slot;
        int art34;
    } job[AZMOD_STEM_DECKS * 8];
    int drum_on = __atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED);
    int nj = 0;
    for (int g = 0; g < AZMOD_STEM_DECKS; g++) {
        /* This grid's tiles show this grid's deck, resolved by identity
         * (deck_for_modeobj, per-deck object +0x10b0). Until the table is up
         * the deck is unknown and the grid stays native. */
        int gdeck = deck_for_modeobj(__atomic_load_n(&g_padgrid_ds[g], __ATOMIC_RELAXED));
        if (gdeck < 0 || gdeck >= AZMOD_STEM_DECKS)
            continue; /* unknown deck: leave this grid native, never another deck's state */
        int armed = __atomic_load_n(&g_drum_held[gdeck], __ATOMIC_RELAXED);
        int sel = (int) (g_drum_sel_led[gdeck] & 3u);
        for (int slot = 0; slot < 8; slot++) {
            int want = -1, disarmed = 0, art34 = -1, hold = 0;
            if (!armed && slot < 7) {
                /* Disarmed: native, except EP147 draws 2*slot while a pad is
                 * held, and the even entries hold our art. Point the pressed
                 * state at the blank so a held pad does not flash a drum
                 * label. Resting (2*slot+1) stays native. */
                disarmed = 1;
            } else if (slot < 4) {
                if (!armed)
                    continue; /* disarmed: leave native */
                want = (sel == slot) ? DRUM_IDX_LIT[slot] : DRUM_IDX_PLAIN[slot];
            } else if (slot == 7) {
                /* disarmed: EP147's default 15 is already our red art.
                 * Mod off: entries 14/15 become the native 3/4 (slip34_apply). */
                art34 = !drum_on;
                if (armed && drum_on)
                    want = DRUM_IDX_TILE8; /* 14 = green */
            } else if (slot == 6) {
                if (!armed)
                    continue; /* disarmed: leave native */
                hold = 1;     /* armed: HOLD, lit while this deck's HOLD is on */
                want = DRUM_IDX_HOLD +
                       !!(__atomic_load_n(&g_drum_hold[gdeck], __ATOMIC_RELAXED) & DRUM_HOLD_ON);
            } else {                       /* slots 4..5 */
                if (!armed)
                    continue;          /* disarmed: leave native */
                want = DRUM_IDX_BLANK; /* armed: blank them */
            }
            long tw = (long) wref_get(WSLOT(&g_slip_tile[g][slot])); /* wref: 0 once destroyed */
            if (tw <= 0x10000)
                continue;
            job[nj].tw = tw;
            job[nj].want = want;
            job[nj].disarmed = disarmed;
            job[nj].hold = hold;
            job[nj].slot = slot;
            job[nj].art34 = art34;
            nj++;
        }
    }
    if (!nj)
        return;
    uintptr_t ad[64], ch[64], v2[64];
    int ok1[64], ok2[64];
    /* each tile's own vtable and its child, in one batch: a tile is used only while it is
     * still a PadStatusWidget (a rebuilt grid frees the ones captured in the ctor) */
    for (int j = 0; j < nj; j++) {
        ad[2 * j] = (uintptr_t) job[j].tw;
        ad[2 * j + 1] = (uintptr_t) job[j].tw + SLIP_CHILD_OFF;
    }
    own_qv(2 * nj, ad, ch, ok1);
    for (int j = 0; j < nj; j++) {
        int live = ok1[2 * j] && (long) ch[2 * j] == (long) PADTILE_VT;
        ok1[j] = live && ok1[2 * j + 1];
        ch[j] = ch[2 * j + 1];
    }
    int n2 = 0, map2[32];
    for (int j = 0; j < nj; j++) {
        if (!ok1[j] || ch[j] <= 0x10000)
            continue;
        map2[n2 / 2] = j;
        ad[n2++] = ch[j];
        ad[n2++] = ch[j] + TILE_MODEL_OFF;
    }
    own_qv(n2, ad, v2, ok2); /* children of tiles proven live and PadStatusWidget above */
    for (int k = 0; k < n2 / 2; k++) {
        int j = map2[k];
        uintptr_t vt = v2[2 * k], model = v2[2 * k + 1];
        if (!ok2[2 * k] || vt != SLIP_WIDGET_VT)
            continue;
        if (!ok2[2 * k + 1] || model <= 0x10000)
            continue;
        int *idx = (int *) (model + MODEL_IDX_OFF);
        if (job[j].art34 >= 0)
            slip34_apply((long) model, job[j].tw, job[j].art34);
        if (job[j].disarmed) {
            if (*idx == 2 * job[j].slot) {
                *idx = DRUM_IDX_BLANK;
                ((void (*)(long)) PAD_REPAINT)(job[j].tw);
            }
            continue;
        }
        if (job[j].want < 0)
            continue; /* tile 8 art only, no index */
        int want = job[j].want;
        if (job[j].hold && !hold_tile_ready((long) model))
            want = DRUM_IDX_BLANK; /* no HOLD art in this array: the blank, as tiles 5-6 */
        if (*idx == want)
            continue;
        *idx = want;
        ((void (*)(long)) PAD_REPAINT)(job[j].tw);
    }
}

/* Per-deck grid for the title refresh, captured from TITLE_UPDATE itself and
 * proven alive by vtable at use (no raw cached pointers). */
static _Atomic long g_title_grid[AZMOD_STEM_DECKS];

static _Atomic long g_title_grid[AZMOD_STEM_DECKS];

static void (*g_orig_titleupd)(long);
/* g is either titleupd_hook's own argument or a non-zero g_title_grid wref slot, both on the
 * message thread: alive, so this checks its class. */
static int title_grid_alive(long g) {
    if (g <= 0x10000 || (g & 7))
        return 0;
    uintptr_t vt;
    if (!own_q((uintptr_t) g, &vt))
        return 0;
    return vt == PADMODESTATUS_VT;
}

/* ---- DRUM ROLL caption, per deck, following the pad-8 latch ----
 * DRUM ROLL shows only while tile 8 is green; otherwise SLIP LOOP. The caption
 * writer caches each decoded image against its source pointer, so rewriting
 * source bytes is invisible after the first decode. Instead we swap the table
 * pointer: PAD_TITLE_TBL entry 3 (SLIP LOOP / KEY SHIFT pages) .normal, in
 * writable .data. Both strips are embedded at distinct addresses, so each has
 * its own cache entry. The arm is exclusive to one deck, so one table slot is
 * unambiguous. The latch edge raises g_caption_repaint; the rtick drain re-runs
 * the writer so the flip is live without a page change. */
static void caption_pointer_set(int armed) {
    unsigned long p =
        armed ? (unsigned long) g_caption_drumroll_png : (unsigned long) g_caption_sliploop_png;
    *(unsigned long *) (EP_3b35f40 + 3 * 16 + 8) = p;
}
static void drum_caption_repaint_if_needed(void) {
    unsigned want = __atomic_exchange_n(&g_caption_repaint, 0, __ATOMIC_ACQ_REL);
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        if (!(want & (1u << d)))
            continue;
        long g = (long) wref_get(WSLOT(&g_title_grid[d]));
        if (!g_orig_titleupd || !title_grid_alive(g))
            continue;
        /* The drain calls the original writer, bypassing titleupd_hook, so
         * set the pointer here for this deck before the re-run. */
        caption_pointer_set(__atomic_load_n(&g_drum_held[d], __ATOMIC_RELAXED) &&
                            __atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED));
        /* The writer re-decodes only when the stored mode (+0xe8) changed.
         * Poison it so the re-run takes the full path; the writer reads the
         * mode fresh from the getter, so the sentinel never reaches art. */
        __atomic_store_n((int *) (g + 0xe8), -1, __ATOMIC_RELAXED);
        g_orig_titleupd(g);
    }
}
static void titleupd_hook(long grid) {
    int tdeck = -1;
    if (title_grid_alive(grid)) {
        tdeck = deck_for_modeobj(*(long *) (grid + 0xc8));
        if (tdeck >= 0 && tdeck < AZMOD_STEM_DECKS) {
            disp_wref_init();
            wref_set(WSLOT(&g_title_grid[tdeck]), (uintptr_t) grid);
        }
    }
    /* Only the SLIP LOOP page (dg_slip_enum) is touched; other modes decode
     * their own art. */
    if (tdeck >= 0 && g_orig_modeget) {
        int tm = g_orig_modeget(*(long *) (grid + 0xc8));
        int held = __atomic_load_n(&g_drum_held[tdeck], __ATOMIC_RELAXED);
        {
            static int lasttm[AZMOD_STEM_DECKS], lastheld[AZMOD_STEM_DECKS];
            static _Atomic int nlog = 400;
            if ((tm != lasttm[tdeck] || held != lastheld[tdeck]) &&
                __atomic_fetch_sub(&nlog, 1, __ATOMIC_RELAXED) > 0) {
                lasttm[tdeck] = tm;
                lastheld[tdeck] = held;
                FILE *f = azlog_open("/tmp/azmod-caption.log", "a");
                if (f) {
                    fprintf(f, "CAPTION d%d tm=%d held=%d arm=%d\n", tdeck, tm, held,
                            __atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED));
                    fclose(f);
                }
            }
        }
        /* azmod-caption-drum.on forces the armed caption without the bus
         * latch (a test aid). Read per call: the writer only runs on
         * mode changes, so this is never hot. */
        int force_cap = AZ_PROBE_ON("/home/root/settings/azmod-caption-drum.on");
        if (tm == dg_slip_enum)
            caption_pointer_set((held && __atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED)) ||
                                force_cap);
    }
    g_orig_titleupd(grid);
}

/* Waveform stem scaling follows the STEMS master (a no-op while every stem is
 * on); /home/root/settings/azmod-wavemute.off forces stock rendering.
 * Hot-path flags: written by the poll thread, read by the hook. Plain ints: a
 * one-frame stale read is harmless on this ~800k calls/s path. */
static int g_wave_any = 0;    /* any waveform mod on at all */
static int g_wave_scale = 0;  /* height scaling (STEMS) */
static int g_wave_color = 0; /* per-column color (STEM COLOR) */
/* 1 = this deck's curve is 256 everywhere (every stem on): scaling is a no-op,
 * so the point hook skips the LUT. Written by build_atten before g_att_have. */
static _Atomic int g_att_ident[AZMOD_STEM_DECKS];
static _Atomic int g_wavemute = 0;
static _Atomic int g_wavecolor = 0; /* STEM COLOR mod */
/* Per-deck attenuation curve, rebuilt off the paint path (overlay thread) when the
 * stem mask, the track or a Stem Volume gain changes. 0..256 fixed point (256 =
 * untouched). Elements are read and written with relaxed __atomic accesses: the
 * message thread reads a curve while it may be rebuilt, and g_att_gen tells it to
 * read the whole curve again. Same for g_col. */
#define ATT_N 16384
static unsigned short g_att[AZMOD_STEM_DECKS][ATT_N];
/* Color of the loudest enabled stem at that point in the track, 0x00RRGGBB,
 * or 0 where nothing is playing (leave EP147's own color alone). */
static unsigned g_col[AZMOD_STEM_DECKS][ATT_N];
static _Atomic int g_att_have[AZMOD_STEM_DECKS];
/* Set when a deck's attenuation curve changes; consumed on the JUCE message
 * thread to force that deck's waveform widgets to repaint immediately. */
static _Atomic int g_wave_dirty[AZMOD_STEM_DECKS];

/* EP147 asks for the waveform one point at a time (consecutive indices): a
 * few points a frame while paused, the whole band on a load, zoom change or
 * seek. The scale comes from the range a full-band redraw covers:
 *
 *     points_per_column = (idx_max - idx_min) / (band_width_px - 1)
 *     playhead_column   = (pos * point_count - idx_min) / points_per_column
 *
 * Measured per redraw burst, never over a longer window: a preview-bar drag
 * repaints the band many times a second at different positions, and a union
 * of those would read as zoomed far out. The playhead anchor also requires a
 * still playhead, so a seek never moves it. */
#define FULLREDRAW 1000 /* points asked for in one tick = a full band */


