/* SPDX-License-Identifier: GPL-3.0-only */
/* azcm.h: the community core's interface to the Mod Menu (community.c; the Community pages read it) and the draw services a mod's page paints through.
 * Included by menu.h (not on its own: it uses menu.h's types). */
#ifndef AZMOD_AZCM_H
#define AZMOD_AZCM_H
/* Shared by azmod.c and menu.c only: hidden, so none of these lands in EP147's symbol
 * namespace (community mods reach the host through az_core_v1, never by name). */
#pragma GCC visibility push(hidden)
/* clang-format off */
/* AZCM_NEW: enabled and would load, but this exact .so is not on the deck's approved list */
enum { AZCM_LOADED = 1, AZCM_DISABLED, AZCM_REFUSED, AZCM_FAILED, AZCM_SUSPENDED, AZCM_NEW };
/* from: where the installed copy came from (the owner/repo of its .origin), "" = copied by hand;
 * allowed: an AZCM_NEW mod the DJ allowed during this start (it loads after the reload) */
typedef struct { char id[33], name[48], version[24], author[48], summary[120], reason[112], from[96];
                 int state, has_screen, hooks, allowed; } azcm_info_t;
int  azcm_count(void);
int  azcm_info(int i, azcm_info_t *out);
int  azcm_suspended(void);
int  azcm_all_off(void);
int  azcm_off_by_stick(void);   /* the stick's MODS/community-mods.off (the app's switch) */
const char *azcm_fwtag(void);
void azcm_screen_draw(int i, az_draw *d, az_rect area);
int  azcm_screen_input(int i, const az_event *ev);
void azcm_screen_enter(int i);
/* the next page opened for mod i shows scr (a SCREEN row's own), NULL = its register_screen page */
void azcm_page_screen(int i, const az_screen_v1 *scr);
/* 1 = a mod's SCREEN row can open something: its own screen, or its running mod's page */
int azcm_row_target(const char *owner, const az_screen_v1 *row_scr);
void azcm_resume(void);
void azcm_flush_settings(void);   /* pending mod settings to p7 now (reload, leaving a mod's page) */
void azcm_reload_requested(void); /* a Mod Menu reload: flush settings; this start counts as healthy */
int azcm_live_set(const char *id, int on); /* the DJ turned a mod on/off: 1 = done now, 0 = after a reload */
void azcm_unsuspend(const char *id);   /* a mod that crashed the deck: load it again next start */
void azcm_set_all_off(int off);
int  azcm_allow(int i);   /* an AZCM_NEW mod: approve the .so this start hashed; 0 = written */
int  azcm_new_count(void); /* AZCM_NEW mods this start, not yet allowed */
/* the paint context a mod's on_draw gets: a framebuffer view clipped to its area (menu.c) */
struct az_draw { azmod_fb fb; int ox, oy, sh; };   /* sh: the SCREEN height, for sizes (fb is clipped) */
void     azcm_draw_header(az_draw *d, const char *title);
void     azcm_draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t rgb);
void     azcm_fill_rect(az_draw *d, int x, int y, int w, int h, uint32_t rgb);
void     azcm_draw_rect(az_draw *d, int x, int y, int w, int h, int width, uint32_t rgb);
void     azcm_draw_checkbox(az_draw *d, int x, int y, int size, int on, int highlighted);
uint32_t azcm_color(int role);
int      azcm_row_height(az_draw *d);
int      azcm_text_width(const char *s, int px);
/* make a paint context for area (device px) of fb: what menu.c hands a mod's on_draw */
void     azcm_draw_init(az_draw *d, const azmod_fb *fb, az_rect area);
/* a Community tile icon scaled to sz x sz (premultiplied ARGB), or NULL until ready / on failure.
 * On NULL, *pending says what to draw: 1 = a decode is queued, draw nothing yet; 0 = draw the
 * placeholder (community_icons.c; decoded on EP147's message thread) */
const uint32_t *azcm_icon(const char *path, int sz, int *pending);
/* a mod's screenshot scaled to fit bw x bh with its aspect kept: its size in *ow x *oh; NULL and
 * *pending as azcm_icon */
const uint32_t *azcm_shot(const char *path, int bw, int bh, int *ow, int *oh, int *pending);
/* how many Community icons have finished (decoded or refused): a change means redraw */
unsigned azcm_icons_done(void);
/* 1 = a mod needing min_core ("" = none) runs on this core (AZ_CORE_VERSION, community.c) */
int azcm_min_core_ok(const char *min_core);
int azcm_ver_newer(const char *a, const char *b);
int azcm_any_loaded(void);   /* a track on any deck, or unknown */
/* A yes/no question from the core itself (the Mod Menu's removal, a handoff), in the dialog the
 * confirm service shows over whatever is on screen: Cancel focused first, BACK = no. fn runs on
 * the overlay thread with the answer. 0 = taken, -1 = another question is open (no answer). */
int azcm_confirm(const char *title, const char *text, const char *yes,
                 void (*fn)(int yes, void *user), void *user);
/* clang-format on */
#pragma GCC visibility pop
#endif
