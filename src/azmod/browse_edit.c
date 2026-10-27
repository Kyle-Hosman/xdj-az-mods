/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file browse_edit.c
 * @brief Column Edit: a COLUMN EDIT button beside EP147's PREVIEW button turns on an edit mode
 * for the Browse track lists' columns.
 *
 * The Mod Menu's Column Edit row gates all of it: off, there is no button, and the saved widths,
 * fields and hidden columns are not applied (EP147's own columns, its own status bar); they are
 * kept for when it is on again. It switches live (be_ce_changed).
 *
 * While editing (the button lit):
 *   - every column that can be hidden shows, and one that is hidden is named OFF;
 *   - a tap on a header no longer sorts: on the sub column it opens EP147's own field list (the
 *     list of the header's sub column button), on a column that can be hidden it hides it or
 *     shows it again (browse_columns.c bc_set_mask; a field list's Hide column does the same);
 *   - a touch near a divider drags it: the column is resized with EP147's own setColumnWidth and
 *     the title column takes what is left.
 * No touch on a header changes the list's sort while editing (be_click_hook).
 * The dividers a touch can drag are marked on the header (be_poc_hook). A RESET button left of
 * COLUMNS, shown only while editing, puts every width, every field and every hidden column back
 * to EP147's own.
 * A second tap on COLUMNS leaves edit mode and saves the widths (settings/azmod-colwidths, the
 * format in browse_edit.h). The widths apply to every full-width track list, after EP147's own
 * layout, so the title keeps at least BE_TITLE_MIN. The compact panes are left as they are.
 * Kill switch: settings/azmod-coledit.off (no button, no saved widths).
 *
 * Everything here runs on EP147's message thread (its constructors, mouse events and layout,
 * and bc_tick).
 *
 * Entry points: install_browse_edit, be_ce_changed (browse_columns.c bc_tick).
 */
/* EP147 (v1.30; v2.00 in the address table):
 * gui::status_area::browse::StatusAreaWidget ctor FUN_01c71728(this, ...) places its children
 *   once: +0xe0 the title (112,0,540,70), +0xf0 the Browse function buttons widget
 *   (674,0,228,70), +0x108 the search bar (102,10,544,50).
 * gui::status_area::browse::BrowseFunctionWidget (vtable 0x2c510a0): +0xc8 previewButton
 *   (0,0,110,70), +0xd0 fontSizeButton (118,0,110,70); both gui::ToggleImagesButton, whose press
 *   calls each listener's slot +0x10 with the button's name. Its controller ignores a name that
 *   is not one of its two buttons.
 *   EP147 shows previewButton only over a track list (its setVisible, ToggleImagesButton vtable
 *   slot +0x60); COLUMNS follows it.
 * gui::browse::TrackListHeader (vtable BC_HDR_VT): slot 5 mouseDown FUN_01bd0878 (sorts by the
 *   column under the touch), slot 6 mouseDrag, slot 7 mouseUp; juce::MouseEvent x at +8.
 *   Its sub column button at +0x138: listeners {+0x210 data, +0x220 count}; the list's own
 *   listener (vtable 0x2b7f6d8) opens EP147's field list from slot +0x10 (this, &name), only
 *   for EP147's own name String 0x3b8e9f8 (it compares the text pointer, not the text).
 * juce::TableHeaderComponent: columns OwnedArray<ColumnInfo> {+0xd8 data, +0xe8 count};
 *   ColumnInfo +0x0 name (juce::String), +0x8 id, +0xc flags (bit 0 visible), +0x10 width;
 *   setColumnWidth 0x248ab00(hdr, id, width), setColumnName 0x248a250(hdr, id, &name).
 * FUN_01bd0600(header): places the header's own sub column button at the right of the sub column
 *   (or of the title while it is hidden); EP147's layout ends with it. */
#include "browse_edit.h"
#include "collabels.h"

#define BE_SA_CTOR EP_1c71728
#define BE_FW_VT EP_2c510a0
#define BE_SETWIDTH EP_248ab00
#define BE_SETNAME EP_248a250
#define BE_PLACE EP_1bd0600 /* TrackListHeader: places its own sub column button on the widths */
#define BE_WLIS_VT EP_2b7f6d8
#define BE_SUBBTN_NAME EP_3b8e9f8 /* the juce::String the list listener compares names with */
#define BE_BUTTON_CTOR EP_24924f0 /* juce::Button(name) */
#define BE_TIMER_CTOR EP_2398900
#define BE_STRING_COPY EP_230e3e0
#define BE_BTN_SETNAME EP_243d1d0
#define BE_TIB_VT0 EP_29674c8 /* gui::ToggleImagesButton */
#define BE_TIB_VT1 EP_2967678
#define BE_TIB_VT2 EP_29676a8
#define BE_ADD_CHILD EP_249d6b0  /* juce::Component::addAndMakeVisible(child, zOrder) */
#define BE_SET_BOUNDS EP_249efe0 /* juce::Component::setBounds(x, y, w, h) */
#define BE_SA_TITLE 0xe0
#define BE_SA_FW 0xf0
#define BE_SA_SEARCH 0x108
#define BE_FW_PREVIEW 0xc8
#define BE_FW_FONT 0xd0
#define BE_HDR_COLS 0xd8
#define BE_HDR_NCOLS 0xe8
#define BE_HDR_SUBBTN 0x138
#define BE_BTN_LIS 0x210
#define BE_ME_X 8
#define BE_SHIFT 118 /* the button and the 8 px gap stock has between PREVIEW and the font buttons */
_Static_assert(BE_SHIFT == BE_SEARCH_STEP, "the search bar gives up BE_SHIFT per button");
#define BE_COLCLICK EP_248bfc0 /* juce::TableHeaderComponent::columnClicked(hdr, id, &mods): the sort toggle */
#define BE_HDR_POC 27 /* juce::Component vtable slot paintOverChildren (EP147's TrackListHeader: empty) */
#define BE_SETCOLOUR EP_23af140 /* juce::Graphics::setColour(g, &argb) */
#define BE_FILLRECT EP_23af680  /* juce::Graphics::fillRect(g, x, y, w, h) */
#define BE_REPAINT EP_2488570   /* juce::Component::repaint() */
#define BE_MARK_ZONE 0x401473ebu /* a divider's touch zone: EP147's selection blue, translucent */
#define BE_MARK_BAR 0xff1473ebu  /* the divider itself */
#define BE_MARK_W 4
#define BE_TAP_SLOP 12
#define BE_FB_MAX 4
#ifndef BE_PATH
#define BE_PATH "/home/root/settings/azmod-colwidths"
#define BE_OFF "/home/root/settings/azmod-coledit.off"
#endif

static int mp_ensure_set(uintptr_t btn, int want_idx, const uintptr_t set[3], int take_extra_ref);
static void mp_image_from_memory(uintptr_t *out, const void *data, int size);

static void be_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-coledit.log", "a");
    if (!f)
        return;
    fprintf(f, "[%ld] ", (long) time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* ---- the header's columns ------------------------------------------------------------------ */
static int be_title_id(void) { return ep_off(4, 5); }
/* EP147's column id for a column bit (bc_col_id) and back */
static unsigned be_bit_of(int id) {
    for (unsigned b = 1; b <= AZMOD_BCOL_ALL; b <<= 1)
        if (bc_col_id(b) == id)
            return b;
    return 0;
}
static unsigned be_fixed_ids(void) {
    unsigned m = 0;
    for (unsigned b = 1; b <= AZMOD_BCOL_ALL; b <<= 1)
        m |= 1u << bc_col_id(b);
    return m;
}
/* every column but the title, which takes what is left (Preview 1) */
static unsigned be_resizable_ids(void) {
    return (1u << 1) | (1u << bc_col_id(AZMOD_BCOL_ART)) | (1u << bc_col_id(AZMOD_BCOL_NUM)) | (1u << bc_sub_id()) |
           (1u << bc_col_id(AZMOD_BCOL_BPM)) | (1u << bc_col_id(AZMOD_BCOL_KEY));
}
/* The header's columns in order, with their ColumnInfo addresses. Returns the count, or -1 if
 * anything read is not as expected. */
static int be_cols(uintptr_t h, struct be_col *c, uintptr_t *ci) {
    uintptr_t data = 0, nq = 0;
    if (!own_q(h + BE_HDR_COLS, &data) || !own_q(h + BE_HDR_NCOLS, &nq) || !data || (data & 7))
        return -1;
    int n = (int) (int32_t) nq;
    if (n < 1 || n > BE_MAXCOLS)
        return -1;
    for (int i = 0; i < n; i++) {
        uintptr_t p = 0, a = 0, b = 0;
        if (!own_q(data + 8 * (uintptr_t) i, &p) || !p || (p & 7) || !own_q(p + 8, &a) || !own_q(p + 0x10, &b))
            return -1;
        c[i].id = (int) (int32_t) a;
        c[i].visible = (int) ((a >> 32) & 1);
        c[i].width = (int) (int32_t) b;
        ci[i] = p;
        if (c[i].id < 1 || c[i].id > BE_MAXID || c[i].width < 0 || c[i].width > 4096)
            return -1;
    }
    return n;
}

/* ---- state (message thread) ---------------------------------------------------------------- */
static int g_be_on = -1;     /* the kill switch, read once (__atomic only) */
static int g_be_edit;        /* edit mode */
static struct be_widths g_be_w; /* the saved widths */
static int g_be_dirty;       /* widths changed since the last save */
static int g_be_relayout;    /* our own setColumnWidth calls: no nested fit */
/* per header while editing: the columns we made visible, and EP147's visibility of them */
static struct be_hdr {
    uintptr_t h;
    unsigned forced, ep_vis;
} g_be_h[BC_HDR_MAX];
/* a touch on a header while editing */
static struct {
    uintptr_t h;
    int active, id, sign, x0, w0, title0, tap_id, moved, partner, p0;
} g_be_t;

static struct be_hdr *be_hdr_state(uintptr_t h, int make) {
    struct be_hdr *free_slot = 0;
    for (int i = 0; i < BC_HDR_MAX; i++) {
        if (g_be_h[i].h == h)
            return &g_be_h[i];
        if (!free_slot && (!g_be_h[i].h || !bc_full_hdr(g_be_h[i].h)))
            free_slot = &g_be_h[i];
    }
    if (!make || !free_slot)
        return 0;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->h = h;
    return free_slot;
}

/* ---- the saved widths ---------------------------------------------------------------------- */
static void be_load(void) {
    char buf[BE_FILE_MAX + 1];
    FILE *f = fopen(BE_PATH, "re");
    if (!f)
        return;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    int rc = be_parse(buf, n, &g_be_w);
    be_log("widths loaded: %d\n", rc);
}
static void be_save(void) {
    char buf[BE_FILE_MAX];
    int n = be_format(&g_be_w, buf, sizeof buf);
    if (n < 0)
        return;
    settings_write(BE_PATH, buf, (size_t) n, 1);
    g_be_dirty = 0;
    be_log("widths saved: %s", buf);
}

/* ---- names: OFF while editing, a field's name, or EP147's own ------------------------------ */
static struct bn_holder g_be_offname;
static struct bn_holder g_be_names[BE_MAXID + 1]; /* EP147's own, kept the first time seen */
static char g_be_last[BE_MAXID + 1][sizeof g_be_offname.text]; /* the name we last gave each column */
static struct bn_holder g_be_fieldname[B2_N];
static void be_set_name(uintptr_t h, int id, struct bn_holder *nm) {
    uintptr_t s = (uintptr_t) nm->text;
    ((void (*)(uintptr_t, int, uintptr_t *)) BE_SETNAME)(h, id, &s);
    snprintf(g_be_last[id], sizeof g_be_last[id], "%s", nm->text);
}
/* the text of a ColumnInfo's name; 0 when it cannot be read whole */
static int be_name_of(uintptr_t ci, char *t, size_t cap) {
    uintptr_t s = 0;
    if (!own_q(ci, &s) || !s)
        return 0;
    for (size_t n = 0; n < cap; n++) {
        uintptr_t q = 0;
        if (!own_q((s + n) & ~(uintptr_t) 7, &q))
            return 0;
        t[n] = (char) (q >> (8 * ((s + n) & 7)));
        if (!t[n])
            return 1;
    }
    return 0;
}
/* every column Column Edit names: the name it should have now, set only when it differs */
static void be_names(uintptr_t h, const struct be_col *c, const uintptr_t *ci, int n, struct be_hdr *s) {
    unsigned m = bc_mask(), set = AZMOD_BCOL_SET_OF(m);
    for (int i = 0; i < n; i++) {
        int id = c[i].id;
        unsigned b = be_bit_of(id), bit = 1u << id;
        char cur[48];
        if ((!b && !bf_swappable(id)) || !be_name_of(ci[i], cur, sizeof cur))
            continue;
        /* any name but one we gave it is EP147's own (a field's label can be one: "Track"); kept
         * once, since the headers EP147 named from it hold its text */
        if (!g_be_names[id].ref && strcmp(cur, g_be_last[id]))
            bn_set(&g_be_names[id], cur, strlen(cur));
        int on = !b || !g_be_edit || !s ? 1 : (set & b) ? (m & b) != 0
                                                        : (s->ep_vis & bit) != 0;
        int f = bf_field_of(id);
        struct bn_holder *want = !on ? &g_be_offname : f != B2_NONE ? &g_be_fieldname[f]
                                                                    : &g_be_names[id];
        if (want->ref && strcmp(cur, want->text))
            be_set_name(h, id, want);
    }
}

/* ---- the layout (bc_layout_hook calls these around EP147's own) --------------------------- */
static void be_before_layout(uintptr_t h) {
    if (!g_be_edit)
        return;
    struct be_hdr *s = be_hdr_state(h, 1);
    if (!s)
        return;
    struct be_col c[BE_MAXCOLS];
    uintptr_t ci[BE_MAXCOLS];
    int n = be_cols(h, c, ci);
    unsigned fixed = be_fixed_ids();
    for (int i = 0; i < n; i++) {
        unsigned bit = 1u << c[i].id;
        if (!(fixed & bit))
            continue;
        if (!c[i].visible) { /* hidden by EP147 or by the DJ: shown while editing */
            s->ep_vis &= ~bit;
            s->forced |= bit;
            g_bc_setvis((long) h, c[i].id, 1);
        } else if (!(s->forced & bit)) {
            s->ep_vis |= bit;
        }
    }
}
static void be_after_layout(uintptr_t h) {
    if (g_be_relayout)
        return;
    struct be_col c[BE_MAXCOLS];
    uintptr_t ci[BE_MAXCOLS];
    int n = be_cols(h, c, ci);
    if (n < 0)
        return;
    struct be_col was[BE_MAXCOLS];
    memcpy(was, c, sizeof(struct be_col) * (size_t) n);
    if (bc_ce_on() && be_fit(c, n, &g_be_w, be_title_id())) {
        g_be_relayout = 1;
        for (int i = 0; i < n; i++)
            if (c[i].visible && c[i].width != was[i].width)
                ((void (*)(uintptr_t, int, int)) BE_SETWIDTH)(h, c[i].id, c[i].width);
        ((void (*)(uintptr_t)) BE_PLACE)(h); /* EP147's own button, on the new widths */
        g_be_relayout = 0;
    }
    be_names(h, c, ci, n, g_be_edit ? be_hdr_state(h, 0) : 0);
    bf_after_layout(h, g_be_edit, BE_ZONE); /* the field buttons: on their columns while editing */
}
/* Edit mode ends: every header as it was, then the columns as the DJ has set them. */
static void be_restore_headers(void) {
    unsigned set = bc_ce_on() ? AZMOD_BCOL_SET_OF(bc_mask()) : 0;
    for (int i = 0; i < BC_HDR_MAX; i++) {
        struct be_hdr *s = &g_be_h[i];
        uintptr_t h = bc_full_hdr(s->h);
        if (h) {
            for (int id = 1; id <= BE_MAXID; id++) {
                unsigned bit = 1u << id;
                if (!(s->forced & bit) || (set & be_bit_of(id)))
                    continue;
                /* EP147's own choice back: as it last set it, else as it was when editing began
                 * (the DJ's own state, for a column the DJ had set) */
                int v = bc_ep_vis(h, id);
                g_bc_setvis((long) h, id, v >= 0 ? v : (s->ep_vis & bit) != 0);
            }
        }
        memset(s, 0, sizeof *s);
    }
}
/* every full-width header on screen laid out again */
static void be_relayout_all(void) { bc_relayout_all(); }
static void be_repaint_headers(void) {
    for (int i = 0; i < BC_HDR_MAX; i++) {
        uintptr_t h = bc_full_hdr(g_bc_hdr[i]);
        if (h)
            ((void (*)(uintptr_t)) BE_REPAINT)(h);
    }
}

/* ---- the COLUMNS button --------------------------------------------------------------------- */
static struct {
    uintptr_t btn, fw, reset, sa;
} g_be_fb[BE_FB_MAX];
static int g_be_parent_off = -1;
static struct bn_holder g_be_name, g_be_rname;
static uintptr_t g_be_img[2][3]; /* label sets: [0] off, [1] on; {normal, down, disabled} */
static uintptr_t g_be_rimg[3];   /* RESET: {normal, down, disabled} */
static int be_btn_live(int i) {
    uintptr_t par = 0;
    return g_be_fb[i].btn && bc_obj(g_be_fb[i].fw, BE_FW_VT) && g_be_parent_off > 0 &&
           own_q(g_be_fb[i].btn + (uintptr_t) g_be_parent_off, &par) && par == g_be_fb[i].fw;
}
/* c is a child of par (juce::Component's parent) */
static int be_child_of(uintptr_t c, uintptr_t par) {
    uintptr_t p = 0;
    return c && !(c & 7) && par && g_be_parent_off > 0 && own_q(c + (uintptr_t) g_be_parent_off, &p) && p == par;
}
static void (*g_o_be_setvis)(long, int);
static int g_be_vis[BE_FB_MAX]; /* COLUMNS shown: as EP147 last set previewButton */
static void be_set_bounds(uintptr_t c, int x, int y, int w, int h);
static int be_bounds(uintptr_t c, int r[4]);
/* c's children (juce::Component +0x40 the array, +0x50 its count), at most cap; their number */
static int be_kids(uintptr_t c, uintptr_t *out, int cap) {
    uintptr_t data = 0, cnt = 0;
    int n = 0;
    if (!own_q(c + 0x40, &data) || !own_q(c + 0x50, &cnt) || !data || (data & 7))
        return 0;
    for (int k = 0; k < (int) (int32_t) cnt && n < cap; k++)
        if (own_q(data + 8 * (uintptr_t) k, &out[n]) && out[n] && !(out[n] & 7))
            n++;
    return n;
}
/* The search bar narrowed by sh: its inner widgets and their children follow (browse_edit.h
 * be_search_kid), so the clear button stays inside the bar. Only EP147's own two inner widget
 * classes, and only children found at their stock places, are moved. */
static void be_search_inner(uintptr_t search, int sh) {
    uintptr_t in[4], kid[8];
    int n = be_kids(search, in, 4);
    for (int i = 0; i < n; i++) {
        int r[4], o[4];
        if (!b2_rtti_is(in[i], "N3gui11status_area6browse21NormalSearchBarWidgetE") &&
            !b2_rtti_is(in[i], "N3gui11status_area6browse21SearchTracksBarWidgetE"))
            continue;
        if (!be_bounds(in[i], r) || !be_search_kid(r, sh, o))
            continue;
        be_set_bounds(in[i], o[0], o[1], o[2], o[3]);
        int m = be_kids(in[i], kid, 8);
        for (int k = 0; k < m; k++)
            if (be_bounds(kid[k], r) && be_search_kid(r, sh, o))
                be_set_bounds(kid[k], o[0], o[1], o[2], o[3]);
    }
}
/* The status area's row: stock with COLUMNS added, and while editing RESET too, left of COLUMNS
 * (COLUMNS stays where it is; the title and the search bar give up the room). */
static void be_layout(int i) {
    uintptr_t fw = g_be_fb[i].fw, sa = g_be_fb[i].sa, pv = 0, font = 0, title = 0, search = 0;
    if (!be_child_of(fw, sa) || !own_q(fw + BE_FW_PREVIEW, &pv) || !be_child_of(pv, fw) ||
        !own_q(fw + BE_FW_FONT, &font) || !be_child_of(font, fw) || !own_q(sa + BE_SA_TITLE, &title) ||
        !be_child_of(title, sa) || !own_q(sa + BE_SA_SEARCH, &search) || !be_child_of(search, sa))
        return;
    int r = g_be_edit && be_child_of(g_be_fb[i].reset, fw) ? BE_SHIFT : 0, sh = bc_ce_on() ? BE_SHIFT + r : 0;
    be_set_bounds(fw, 674 - sh, 0, 228 + sh, 70);
    if (r)
        be_set_bounds(g_be_fb[i].reset, 0, 0, 110, 70);
    if (sh)
        be_set_bounds(g_be_fb[i].btn, r, 0, 110, 70);
    be_set_bounds(pv, sh, 0, 110, 70);
    be_set_bounds(font, 118 + sh, 0, 110, 70);
    be_set_bounds(title, 112, 0, 540 - sh, 70);
    be_set_bounds(search, 102, 10, 544 - sh, 50);
    be_search_inner(search, sh);
}
static void be_paint(int i) {
    *(int32_t *) (g_be_fb[i].btn + 0x1e0) = g_be_edit; /* the label set: lit while editing */
    ((void (*)(uintptr_t)) BE_REPAINT)(g_be_fb[i].btn);
    be_layout(i);
    if (g_o_be_setvis) {
        g_o_be_setvis((long) g_be_fb[i].btn, g_be_vis[i] && bc_ce_on());
        if (be_child_of(g_be_fb[i].reset, g_be_fb[i].fw))
            g_o_be_setvis((long) g_be_fb[i].reset, g_be_vis[i] && g_be_edit);
    }
}
static void be_show_buttons(void) {
    for (int i = 0; i < BE_FB_MAX; i++)
        if (be_btn_live(i))
            be_paint(i);
}
/* ToggleImagesButton::setVisible: on a previewButton of ours, COLUMNS shows or hides with it */
static void be_setvis_hook(long btn, int vis) {
    g_o_be_setvis(btn, vis);
    for (int i = 0; i < BE_FB_MAX; i++) {
        uintptr_t pv = 0;
        if (g_be_fb[i].btn && (uintptr_t) btn != g_be_fb[i].btn && (uintptr_t) btn != g_be_fb[i].reset &&
            be_btn_live(i) &&
            own_q(g_be_fb[i].fw + BE_FW_PREVIEW, &pv) && pv == (uintptr_t) btn) {
            g_be_vis[i] = (vis & 0xff) != 0;
            be_paint(i);
        }
    }
}
static void be_set_edit(int on) {
    if (on == g_be_edit)
        return;
    g_be_edit = on;
    g_be_t.active = 0;
    g_be_t.h = 0;
    if (!on) {
        be_restore_headers();
        if (g_be_dirty)
            be_save();
    }
    be_show_buttons();
    be_relayout_all();
    be_repaint_headers();
    be_log("edit mode %d\n", on);
}
/* RESET: every saved width, every field and every hidden or shown column back to EP147's own,
 * saved at once; EP147's layout gives the widths back. Edit mode stays on. */
static void be_reset(void) {
    memset(&g_be_w, 0, sizeof g_be_w);
    g_be_t.active = 0;
    g_be_t.h = 0;
    be_save();
    bf_reset();
    bc_set_mask(AZMOD_BCOL_DEFAULT);
    be_relayout_all();
    be_repaint_headers();
    be_log("reset\n");
}
/* our listener on the button: slot +0x10 is the press (this, &name) */
struct be_lis {
    const void *vt;
};
static void be_lis_nop(void *self) { (void) self; }
static void be_lis_press(void *self, void *name) {
    (void) self;
    (void) name;
    if (bc_ce_on())
        be_set_edit(!g_be_edit);
}
static void be_rlis_press(void *self, void *name) {
    (void) self;
    (void) name;
    if (g_be_edit)
        be_reset();
}
static void (*const g_be_lis_vt[8])(void) = {
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_lis_press,
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_lis_nop,
};
static void (*const g_be_rlis_vt[8])(void) = {
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_rlis_press,
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_lis_nop,
    (void (*)(void)) be_lis_nop,
};
static struct be_lis g_be_lis = {g_be_lis_vt}, g_be_rlis = {g_be_rlis_vt};

static int be_load_images(void) {
    static int done;
    if (done)
        return done > 0;
    static const unsigned char *const src[2][3] = {
        {CL_COLUMNS_normal, CL_COLUMNS_normal, CL_COLUMNS_disabled},
        {CL_COLUMNS_on, CL_COLUMNS_on, CL_COLUMNS_disabled}};
    static const int len[2][3] = {
        {sizeof CL_COLUMNS_normal, sizeof CL_COLUMNS_normal, sizeof CL_COLUMNS_disabled},
        {sizeof CL_COLUMNS_on, sizeof CL_COLUMNS_on, sizeof CL_COLUMNS_disabled}};
    for (int k = 0; k < 2; k++)
        for (int s = 0; s < 3; s++) {
            uintptr_t img = 0, q = 0;
            mp_image_from_memory(&img, src[k][s], len[k][s]);
            if (!img || !own_q(img + 0x10, &q) || (uint32_t) q != 110 || (uint32_t) (q >> 32) != 70) {
                be_log("label %d/%d failed (%lx)\n", k, s, (unsigned long) img);
                done = -1;
                return 0;
            }
            g_be_img[k][s] = img;
        }
    static const unsigned char *const rsrc[3] = {CL_RESET_normal, CL_RESET_on, CL_RESET_disabled};
    static const int rlen[3] = {sizeof CL_RESET_normal, sizeof CL_RESET_on, sizeof CL_RESET_disabled};
    for (int s = 0; s < 3; s++) {
        uintptr_t img = 0, q = 0;
        mp_image_from_memory(&img, rsrc[s], rlen[s]);
        if (!img || !own_q(img + 0x10, &q) || (uint32_t) q != 110 || (uint32_t) (q >> 32) != 70) {
            be_log("reset label %d failed (%lx)\n", s, (unsigned long) img);
            done = -1;
            return 0;
        }
        g_be_rimg[s] = img;
    }
    done = 1;
    return 1;
}
/* A gui::ToggleImagesButton of our own on the function widget: the inline sequence EP147's
 * widget constructor uses for previewButton (menu_popup.c mp_make_btn has the details). */
static uintptr_t be_make_btn(uintptr_t fw, struct bn_holder *nm, struct be_lis *lis, const uintptr_t *set0,
                             const uintptr_t *set1) {
    uintptr_t name = (uintptr_t) nm->text;
    /* everything allocated before the component is built: a failure leaves nothing half-made */
    uintptr_t *ls = (uintptr_t *) malloc(8 * sizeof(uintptr_t));
    uintptr_t *arr = (uintptr_t *) malloc(3 * 0x18);
    uintptr_t p = (uintptr_t) calloc(1, 0x220);
    if (!ls || !arr || !p) {
        free(ls);
        free(arr);
        free((void *) p);
        return 0;
    }
    ((void (*)(uintptr_t, uintptr_t *)) BE_BUTTON_CTOR)(p, &name);
    ((void (*)(uintptr_t)) BE_TIMER_CTOR)(p + 0x1b8);
    *(uintptr_t *) (p + 0x000) = BE_TIB_VT0;
    *(uintptr_t *) (p + 0x0c8) = BE_TIB_VT1;
    *(uintptr_t *) (p + 0x1b8) = BE_TIB_VT2;
    ((void (*)(uintptr_t, uintptr_t *)) BE_STRING_COPY)(p + 0x1d8, &name);
    *(int32_t *) (p + 0x204) = 500;
    ls[0] = (uintptr_t) lis;
    *(uintptr_t *) (p + 0x208) = (uintptr_t) ls;
    *(int32_t *) (p + 0x210) = 8;
    *(int32_t *) (p + 0x218) = 1;
    ((void (*)(uintptr_t, uintptr_t *)) BE_BTN_SETNAME)(p, &name);
    *(uintptr_t *) (p + 0x1e8) = (uintptr_t) arr;
    *(int32_t *) (p + 0x1f0) = 3;
    *(int32_t *) (p + 0x1f8) = 0;
    if (!mp_ensure_set(p, 0, set0, 1) || (set1 && !mp_ensure_set(p, 1, set1, 1)))
        return 0;
    ((void (*)(uintptr_t, uintptr_t, int)) BE_ADD_CHILD)(fw, p, -1);
    return p;
}
/* juce::Component's bounds: found once, on previewButton, as the rectangle the widget gave it */
static int g_be_bounds_off = -1;
static int be_bounds(uintptr_t c, int r[4]) {
    uintptr_t a = 0, b = 0;
    if (g_be_bounds_off <= 0 || !own_q(c + (uintptr_t) g_be_bounds_off, &a) ||
        !own_q(c + (uintptr_t) g_be_bounds_off + 8, &b))
        return 0;
    r[0] = (int) (int32_t) a;
    r[1] = (int) (int32_t) (a >> 32);
    r[2] = (int) (int32_t) b;
    r[3] = (int) (int32_t) (b >> 32);
    return 1;
}
static int be_is(uintptr_t c, int x, int y, int w, int h) {
    int r[4];
    return c && !(c & 7) && be_bounds(c, r) && r[0] == x && r[1] == y && r[2] == w && r[3] == h;
}
static void be_set_bounds(uintptr_t c, int x, int y, int w, int h) {
    ((void (*)(uintptr_t, int, int, int, int)) BE_SET_BOUNDS)(c, x, y, w, h);
}
static void (*g_o_be_sa_ctor)(long, long, long, long, long, long, long, long);
static void be_sa_ctor_hook(long sa, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
    g_o_be_sa_ctor(sa, a1, a2, a3, a4, a5, a6, a7);
    uintptr_t fw = 0, title = 0, search = 0, pv = 0, font = 0;
    if (!own_q((uintptr_t) sa + BE_SA_FW, &fw) || !bc_obj(fw, BE_FW_VT) ||
        !own_q((uintptr_t) sa + BE_SA_TITLE, &title) || !own_q((uintptr_t) sa + BE_SA_SEARCH, &search) ||
        !own_q(fw + BE_FW_PREVIEW, &pv) || !bc_obj(pv, BE_TIB_VT0) || !own_q(fw + BE_FW_FONT, &font) ||
        !bc_obj(font, BE_TIB_VT0)) {
        be_log("status area %lx: not as expected\n", (unsigned long) sa);
        return;
    }
    if (g_be_bounds_off < 0) { /* the offset where previewButton holds (0, 0, 110, 70) */
        for (int o = 0x8; o < 0x100 && g_be_bounds_off < 0; o += 8) {
            uintptr_t a = 0, b = 0;
            if (own_q(pv + (uintptr_t) o, &a) && own_q(pv + (uintptr_t) o + 8, &b) && a == 0 &&
                b == ((uintptr_t) 70 << 32 | 110))
                g_be_bounds_off = o;
        }
        for (int o = 0x8; o < 0x100 && g_be_parent_off < 0; o += 8) {
            uintptr_t v = 0;
            if (own_q(pv + (uintptr_t) o, &v) && v == fw)
                g_be_parent_off = o;
        }
    }
    /* stock layout only: anything else is left exactly as EP147 made it */
    if (!be_is(fw, 674, 0, 228, 70) || !be_is(pv, 0, 0, 110, 70) || !be_is(font, 118, 0, 110, 70) ||
        !be_is(title, 112, 0, 540, 70) || !be_is(search, 102, 10, 544, 50) || g_be_parent_off <= 0 ||
        !be_load_images()) {
        be_log("status area %lx: layout not stock (bounds at %d, parent at %d)\n", (unsigned long) sa,
               g_be_bounds_off, g_be_parent_off);
        return;
    }
    uintptr_t btn = be_make_btn(fw, &g_be_name, &g_be_lis, g_be_img[0], g_be_img[1]);
    if (!btn) {
        be_log("button not made\n");
        return;
    }
    uintptr_t reset = be_make_btn(fw, &g_be_rname, &g_be_rlis, g_be_rimg, 0);
    if (!reset)
        be_log("RESET not made\n");
    int slot = 0;
    for (int i = 0; i < BE_FB_MAX; i++)
        if (!be_btn_live(i)) {
            slot = i;
            break;
        }
    g_be_fb[slot].btn = btn;
    g_be_fb[slot].fw = fw;
    g_be_fb[slot].reset = reset;
    g_be_fb[slot].sa = (uintptr_t) sa;
    g_be_vis[slot] = 0; /* until EP147 shows previewButton */
    be_paint(slot);
    be_log("COLUMNS button %lx on %lx\n", (unsigned long) btn, (unsigned long) fw);
}

/* ---- the header's mouse --------------------------------------------------------------------- */
static void (*g_o_be_down)(long, long), (*g_o_be_drag)(long, long), (*g_o_be_up)(long, long);
static int be_mouse_x(long e) {
    uintptr_t q = 0;
    return own_q((uintptr_t) e + BE_ME_X, &q) ? (int) (int32_t) q : -1;
}
static int be_mouse_y(long e) {
    uintptr_t q = 0;
    return own_q((uintptr_t) e + BE_ME_X, &q) ? (int) (int32_t) (q >> 32) : -1;
}
static int be_column_at(const struct be_col *c, int n, int x) {
    int edge = 0;
    for (int i = 0; i < n; i++) {
        if (!c[i].visible)
            continue;
        if (x >= edge && x < edge + c[i].width)
            return c[i].id;
        edge += c[i].width;
    }
    return 0;
}
static void be_touch_at(uintptr_t h, int x);
/* a press at x on header h in edit mode: a divider in reach starts a drag, anything else is a tap
 * when it does not move. 1 when a divider took it. */
static int be_press(uintptr_t h, const struct be_col *c, int n, int x) {
    int sign = 0, partner = 0;
    int id = be_divider(c, n, x, be_resizable_ids(), be_title_id(), &sign, &partner);
    memset(&g_be_t, 0, sizeof g_be_t);
    g_be_t.h = h;
    g_be_t.x0 = x;
    if (id) {
        for (int i = 0; i < n; i++) {
            if (c[i].id == id)
                g_be_t.w0 = c[i].width;
            if (partner && c[i].id == partner)
                g_be_t.p0 = c[i].width;
            if (c[i].id == be_title_id())
                g_be_t.title0 = c[i].width;
        }
        g_be_t.active = 1;
        g_be_t.id = id;
        g_be_t.sign = sign;
        g_be_t.partner = partner;
    }
    g_be_t.tap_id = be_column_at(c, n, x); /* a touch near a divider that does not move is a tap */
    return id != 0;
}
static void be_down_hook(long hdr, long e) {
    uintptr_t h = bc_full_hdr((uintptr_t) hdr);
    struct be_col c[BE_MAXCOLS];
    uintptr_t ci[BE_MAXCOLS];
    int n = h ? be_cols(h, c, ci) : -1, x = be_mouse_x(e);
    if (!g_be_edit) {
        /* a column showing a field: EP147 would sort by the column's own data, which it does not show */
        if (n < 0 || x < 0 || bf_field_of(be_column_at(c, n, x)) == B2_NONE)
            g_o_be_down(hdr, e);
        return;
    }
    if (!h || n < 0 || x < 0)
        return;
    b2_gest(0, x, be_mouse_y(e));
    /* a press while a drag is going on (the deck repeats the press as a touch moves): the same
     * drag, moved to this press */
    if (g_be_t.active && g_be_t.h == h) {
        be_touch_at(h, x);
        return;
    }
    be_press(h, c, n, x);
}
/* the touch at x while a header is touched in edit mode */
static void be_touch_at(uintptr_t h, int x) {
    if (x < 0 || g_be_t.h != h)
        return;
    if (x - g_be_t.x0 > BE_TAP_SLOP || g_be_t.x0 - x > BE_TAP_SLOP)
        g_be_t.moved = 1;
    if (!g_be_t.active || !g_be_t.moved)
        return; /* a drag starts past BE_TAP_SLOP: a still touch stays a tap */
    int pw = 0, w;
    if (g_be_t.partner) { /* two resizable neighbors trade width: the title keeps its own */
        w = be_trade_width(g_be_t.w0, g_be_t.p0, x - g_be_t.x0, g_be_t.sign, &pw);
        if (w == g_be_w.w[g_be_t.id] && pw == g_be_w.w[g_be_t.partner])
            return;
        g_be_w.w[g_be_t.partner] = (int16_t) pw;
    } else {
        w = be_drag_width(g_be_t.w0, g_be_t.title0, x - g_be_t.x0, g_be_t.sign);
        if (w == g_be_w.w[g_be_t.id])
            return;
    }
    g_be_w.w[g_be_t.id] = (int16_t) w;
    g_be_dirty = 1;
    be_after_layout(h); /* the column takes it, the title the rest */
}
static void be_drag_hook(long hdr, long e) {
    uintptr_t h = g_be_edit ? bc_full_hdr((uintptr_t) hdr) : 0;
    if (!h) {
        g_o_be_drag(hdr, e);
        return;
    }
    b2_gest(1, be_mouse_x(e), be_mouse_y(e));
    be_touch_at(h, be_mouse_x(e));
}
/* A field button (browse_fields.c) pressed at header x h: the press is the header's when a divider
 * is in reach (a drag, or a tap that does not move), and the button's otherwise. 1 = the header's;
 * the button's drag and release then come here too. A press while that drag goes on moves it. */
static int be_btn_press(uintptr_t h, int x) {
    struct be_col c[BE_MAXCOLS];
    uintptr_t ci[BE_MAXCOLS];
    int n = g_be_edit && bc_full_hdr(h) ? be_cols(h, c, ci) : -1;
    if (n < 0 || x < 0)
        return 0;
    if (g_be_t.active && g_be_t.h == h) {
        be_touch_at(h, x);
        return 1;
    }
    int sign = 0, partner = 0;
    if (!be_divider(c, n, x, be_resizable_ids(), be_title_id(), &sign, &partner))
        return 0;
    be_press(h, c, n, x);
    return 1;
}
static void be_btn_drag(uintptr_t h, int x) { be_touch_at(h, x); }
/* the release of a press be_btn_press took: 1 when it dragged, 0 for a still touch (the button's
 * own action then) */
static int be_btn_release(uintptr_t h) {
    int dragged = g_be_t.h == h && g_be_t.active && g_be_t.moved;
    if (dragged)
        be_log("column %d width %d, column %d width %d\n", g_be_t.id, g_be_w.w[g_be_t.id], g_be_t.partner,
               g_be_t.partner ? g_be_w.w[g_be_t.partner] : 0);
    g_be_t.active = 0;
    return dragged || (g_be_t.h == h && g_be_t.moved);
}
static int be_editing(void) { return g_be_edit; }
/* EP147's own field list, as the header's sub column button opens it */
static void be_open_fields(uintptr_t h) {
    uintptr_t btn = 0, data = 0, nq = 0;
    if (!own_q(h + BE_HDR_SUBBTN, &btn) || !btn || (btn & 7) || !own_q(btn + BE_BTN_LIS, &data) ||
        !own_q(btn + BE_BTN_LIS + 0x10, &nq) || !data)
        return;
    for (int i = 0; i < (int) (int32_t) nq && i < 16; i++) {
        uintptr_t l = 0, lvt = 0, fn = 0;
        if (!own_q(data + 8 * (uintptr_t) i, &l) || !l || (l & 7) || !own_q(l, &lvt) || lvt != BE_WLIS_VT ||
            !img_q(lvt + 0x10, &fn) || !fn)
            continue;
        uintptr_t text = 0;
        if (!BE_SUBBTN_NAME || !own_q(BE_SUBBTN_NAME, &text) || !text)
            return;
        ((void (*)(uintptr_t, uintptr_t)) fn)(l, BE_SUBBTN_NAME);
        be_log("field list opened\n");
        return;
    }
    be_log("field list: no listener on %lx\n", (unsigned long) btn);
}
static void be_up_hook(long hdr, long e) {
    uintptr_t h = g_be_edit ? bc_full_hdr((uintptr_t) hdr) : 0;
    if (!h) {
        g_o_be_up(hdr, e);
        return;
    }
    b2_gest(2, be_mouse_x(e), be_mouse_y(e));
    if (g_be_t.h != h)
        return;
    int dragged = g_be_t.active && g_be_t.moved;
    g_be_t.active = 0;
    if (dragged)
        be_log("column %d width %d, column %d width %d\n", g_be_t.id, g_be_w.w[g_be_t.id], g_be_t.partner,
               g_be_t.partner ? g_be_w.w[g_be_t.partner] : 0);
    if (g_be_t.moved)
        return;
    int id = g_be_t.tap_id;
    switch (be_tap(1, id, be_fixed_ids(), bc_sub_id())) {
    case BE_TAP_FIELD:
        be_open_fields(h);
        break;
    case BE_TAP_TOGGLE: {
        unsigned b = be_bit_of(id), m = bc_mask();
        struct be_hdr *s = be_hdr_state(h, 0);
        int on = (AZMOD_BCOL_SET_OF(m) & b) ? (m & b) != 0 : s && (s->ep_vis & (1u << id));
        bc_set_mask((on ? m & ~b : m | b) | AZMOD_BCOL_SET(b));
        be_log("column %d %s\n", id, on ? "off" : "on");
        break;
    }
    default:
        break;
    }
}

/* columnClicked toggles a column's sort: EP147's mouseDown calls it, and so does JUCE's mouseUp.
 * While editing, a full-width header's touches are edits, so the sort stays as it is. */
static void (*g_o_be_click)(long, int, long);
static void be_click_hook(long hdr, int id, long mods) {
    if (g_be_edit && bc_full_hdr((uintptr_t) hdr)) {
        be_log("sort kept: column %d\n", id);
        return;
    }
    g_o_be_click(hdr, id, mods);
}

/* The header's paintOverChildren, after EP147's (an empty one): while editing, each divider a touch
 * can drag is marked, its touch zone tinted and the divider drawn as a bar, over the buttons. */
static void (*g_o_be_poc)(long, long);
static void be_poc_hook(long hdr, long g) {
    g_o_be_poc(hdr, g);
    uintptr_t h = g_be_edit ? bc_full_hdr((uintptr_t) hdr) : 0;
    struct be_col c[BE_MAXCOLS];
    uintptr_t ci[BE_MAXCOLS];
    int r[4], xs[BE_MAXCOLS];
    int n = h ? be_cols(h, c, ci) : -1;
    if (n < 0 || !g || !be_bounds(h, r) || r[3] <= 0)
        return;
    int k = be_handles(c, n, be_resizable_ids(), be_title_id(), xs, BE_MAXCOLS);
    uint32_t zone = BE_MARK_ZONE, bar = BE_MARK_BAR;
    for (int i = 0; i < k; i++) {
        ((void (*)(long, const uint32_t *)) BE_SETCOLOUR)(g, &zone);
        ((void (*)(long, int, int, int, int)) BE_FILLRECT)(g, xs[i] - BE_ZONE, 0, 2 * BE_ZONE, r[3]);
        ((void (*)(long, const uint32_t *)) BE_SETCOLOUR)(g, &bar);
        ((void (*)(long, int, int, int, int)) BE_FILLRECT)(g, xs[i] - BE_MARK_W / 2, 0, BE_MARK_W, r[3]);
    }
}

/* browse_columns.c: the Mod Menu's Column Edit row went on or off. Off: edit mode ends (the widths
 * saved), the button goes and the status bar is EP147's again; the headers lay out again without
 * the saved widths, fields and names, and the lists repaint with their own data. */
static void be_ce_changed(int on) {
    if (!on)
        be_set_edit(0);
    be_show_buttons();
    __atomic_store_n(&g_b2_layout, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_b2_refresh, 1, __ATOMIC_RELEASE);
    be_log("column edit %d\n", on);
}

static void install_browse_edit(void) {
    if (!__atomic_load_n(&g_be_on, __ATOMIC_ACQUIRE) || access(BE_OFF, F_OK) == 0) {
        __atomic_store_n(&g_be_on, 0, __ATOMIC_RELEASE);
        be_log("off (kill switch)\n");
        return;
    }
    bn_set(&g_be_name, "AZ_COLUMNS", 10);
    bn_set(&g_be_rname, "AZ_COLRESET", 11);
    bn_set(&g_be_offname, "OFF", 3);
    for (int f = 1; f < B2_N; f++)
        bn_set(&g_be_fieldname[f], B2_FIELDS[f].label, strlen(B2_FIELDS[f].label));
    be_load();
    g_bc_before_layout = be_before_layout;
    g_bc_after_layout = be_after_layout;
    g_bc_ce_changed = be_ce_changed;
    int a = BE_SA_CTOR && BE_FW_VT && azm_install_hook(BE_SA_CTOR, (void *) be_sa_ctor_hook, (void **) &g_o_be_sa_ctor);
    int b = BC_HDR_VT && azm_patch_vtable(BC_HDR_VT + 5 * 8, (void *) be_down_hook, (void **) &g_o_be_down) == 1 &&
            azm_patch_vtable(BC_HDR_VT + 6 * 8, (void *) be_drag_hook, (void **) &g_o_be_drag) == 1 &&
            azm_patch_vtable(BC_HDR_VT + 7 * 8, (void *) be_up_hook, (void **) &g_o_be_up) == 1;
    int v = a && azm_patch_vtable(BE_TIB_VT0 + 0x60, (void *) be_setvis_hook, (void **) &g_o_be_setvis) == 1;
    int m = b && azm_patch_vtable(BC_HDR_VT + BE_HDR_POC * 8, (void *) be_poc_hook, (void **) &g_o_be_poc) == 1;
    int s = BE_COLCLICK && azm_install_hook(BE_COLCLICK, (void *) be_click_hook, (void **) &g_o_be_click);
    __atomic_store_n(&g_be_on, 1, __ATOMIC_RELEASE);
    be_log("installed: button=%d mouse=%d visibility=%d marks=%d sort=%d\n", a, b, v, m, s);
}
