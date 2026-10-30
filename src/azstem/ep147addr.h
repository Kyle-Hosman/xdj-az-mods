/* SPDX-License-Identifier: GPL-3.0-only */
/* Generated per firmware build. Do not edit by hand. 34 EP147 addresses for azstem.
 *
 * Code names an EP147 address by its v1.30 value (EP_1b93a28). ep147_addr_init(build)
 * fills ep147_addr[] for the running build, which common/ep147build.h
 * identified by md5; until then, and on an unknown build, every entry is 0.
 * v2.00 values are hand-resolved where the automatic re-anchor was ambiguous. */
#ifndef EP147ADDR_H
#define EP147ADDR_H
#include "../common/ep147build.h"

enum {
    EPI_7670a8,
    EPI_77e260,
    EPI_7820d0,
    EPI_782dc0,
    EPI_783010,
    EPI_783fe0,
    EPI_784e90,
    EPI_785228,
    EPI_d58ee0,
    EPI_d9c9d8,
    EPI_d9ea00,
    EPI_d9f320,
    EPI_da07d8,
    EPI_da2f68,
    EPI_da3df0,
    EPI_dabb48,
    EPI_1381538,
    EPI_14a2270,
    EPI_14a2560,
    EPI_14ab0d0,
    EPI_1f91210,
    EPI_1f9c498,
    EPI_1fa16d0,
    EPI_22043f0,
    EPI_25f9c00,
    EPI_2754518,
    EPI_2798328,
    EPI_27b8a50,
    EPI_27ba108,
    EPI_27bb148,
    EPI_289f798,
    EPI_289f878,
    EPI_2dd9288,
    EPI_2ddb878,
    EPI_N
};
static const uint32_t ep147_addr_v130[EPI_N] = {
    0x07670a8,
    0x077e260,
    0x07820d0,
    0x0782dc0,
    0x0783010,
    0x0783fe0,
    0x0784e90,
    0x0785228,
    0x0d58ee0,
    0x0d9c9d8,
    0x0d9ea00,
    0x0d9f320,
    0x0da07d8,
    0x0da2f68,
    0x0da3df0,
    0x0dabb48,
    0x1381538,
    0x14a2270,
    0x14a2560,
    0x14ab0d0,
    0x1f91210,
    0x1f9c498,
    0x1fa16d0,
    0x22043f0,
    0x25f9c00,
    0x2754518,
    0x2798328,
    0x27b8a50,
    0x27ba108,
    0x27bb148,
    0x289f798,
    0x289f878,
    0x2dd9288,
    0x2ddb878,
};
static const uint32_t ep147_addr_v200[EPI_N] = {
    0x07a8928,   /* code w=12 | xref 1/1 agree | ptr 1 agree */
    0x07bfb60,   /* code w=12 | xref 3/3 agree */
    0x07c39d0,   /* code w=12 | ptr 1 agree */
    0x07c46c0,   /* code w=12 | xref 2/2 agree */
    0x07c4910,   /* code w=12 | xref 3/3 agree */
    0x07c58e0,   /* code w=12 | ptr 1 agree */
    0x07c6790,   /* code w=12 | xref 1/1 agree */
    0x07c6b28,   /* code w=12 | xref 2/2 agree */
    0x0de3c08,   /* code w=12 | ptr 1 agree */
    0x0e277e0,   /* code w=12 | ptr 1 agree */
    0x0e29660,   /* code w=12 | xref 3/3 agree */
    0x0e29f80,   /* code w=12 | xref 1/1 agree */
    0x0e2b4f0,   /* code w=12 | xref 2/2 agree */
    0x0e2dc80,   /* code w=12 | xref 1/1 agree | ptr 1 agree */
    0x0e2eb28,   /* code w=12 | xref 1/1 agree | ptr 1 agree */
    0x0e365a8,   /* code w=12 | ptr 1 agree */
    0x143c760,   /* code w=12 | ptr 1 agree */
    0x155d700,   /* hand: RTTI: slot 0 (complete-object destructor) of the vtable whose type_info names N9dj_p */
    0x155d9f0,   /* code w=12 | xref 1/1 agree */
    0x1566500,   /* code w=12 | ptr 4 agree */
    0x20fd5e0,   /* hand: RTTI: gui::LoadedTrackInfo vtable (1.30 0x2dd9288, 2.00 0x318c260): 1.30 slot +0x88, */
    0x2109d20,   /* code w=12 | xref 1/1 agree */
    0x210edf0,   /* hand: RTTI: slot 0 (complete-object destructor) of the vtable whose type_info names N3gui9 */
    0x2371890,   /* hand: masked code signature (12 words, unique) and 6/6 sampled callers agree; the function */
    0x2933258,   /* vtable N6pcmbuf7pagebuf10PageBufferINS0_23DefaultPageStaticConfigEEE +0x0 | xref 3/3 agree */
    0x2a9cfa8,   /* hand: meow::UpdatableSharedObject<appnd_trk_info::BeatGrid> vtable (vtable relocation, RTT */
    0x2ae4df8,   /* hand: appnd_trk_info::BeatGrid::Content vtable (vtable relocation, RTTI) */
    0x2b05708,   /* vtable N11time_domain26CascadedTimeStretchManagerE +0x0 | xref 2/2 agree */
    0x2b06dd0,   /* vtable N11time_domain14KeyControlFGPRE +0x0 | xref 2/2 agree */
    0x2b07de8,   /* vtable N11time_domain11TimeStretchE +0x90 */
    0x2bf2268,   /* vtable N12player_basis15PlayerBasisUnitE +0x0 | xref 2/2 agree */
    0x2bf2348,   /* vtable N9dj_player12DjPlayerUnitE +0x0 | xref 2/2 agree */
    0x318c260,   /* vtable N3gui15LoadedTrackInfoE +0x0 */
    0x318ec68,   /* hand: RTTI: the primary vtable (offset-to-top 0) whose type_info names N3gui9LoadStateE; t */
};
static uintptr_t ep147_addr[EPI_N];

/* 1 = every address this library uses exists in `build`; 0 = stay inert. */
static int ep147_addr_init(int build){
    const uint32_t *t = build == EPB_V130 ? ep147_addr_v130 : build == EPB_V200 ? ep147_addr_v200 : 0;
    if(!t) return 0;
    for(int i = 0; i < EPI_N; i++){ if(!t[i]) return 0; }
    for(int i = 0; i < EPI_N; i++) ep147_addr[i] = t[i];
    return 1;
}

#define EP_7670a8 ((uintptr_t)ep147_addr[EPI_7670a8])
#define EP_77e260 ((uintptr_t)ep147_addr[EPI_77e260])
#define EP_7820d0 ((uintptr_t)ep147_addr[EPI_7820d0])
#define EP_782dc0 ((uintptr_t)ep147_addr[EPI_782dc0])
#define EP_783010 ((uintptr_t)ep147_addr[EPI_783010])
#define EP_783fe0 ((uintptr_t)ep147_addr[EPI_783fe0])
#define EP_784e90 ((uintptr_t)ep147_addr[EPI_784e90])
#define EP_785228 ((uintptr_t)ep147_addr[EPI_785228])
#define EP_d58ee0 ((uintptr_t)ep147_addr[EPI_d58ee0])
#define EP_d9c9d8 ((uintptr_t)ep147_addr[EPI_d9c9d8])
#define EP_d9ea00 ((uintptr_t)ep147_addr[EPI_d9ea00])
#define EP_d9f320 ((uintptr_t)ep147_addr[EPI_d9f320])
#define EP_da07d8 ((uintptr_t)ep147_addr[EPI_da07d8])
#define EP_da2f68 ((uintptr_t)ep147_addr[EPI_da2f68])
#define EP_da3df0 ((uintptr_t)ep147_addr[EPI_da3df0])
#define EP_dabb48 ((uintptr_t)ep147_addr[EPI_dabb48])
#define EP_1381538 ((uintptr_t)ep147_addr[EPI_1381538])
#define EP_14a2270 ((uintptr_t)ep147_addr[EPI_14a2270])
#define EP_14a2560 ((uintptr_t)ep147_addr[EPI_14a2560])
#define EP_14ab0d0 ((uintptr_t)ep147_addr[EPI_14ab0d0])
#define EP_1f91210 ((uintptr_t)ep147_addr[EPI_1f91210])
#define EP_1f9c498 ((uintptr_t)ep147_addr[EPI_1f9c498])
#define EP_1fa16d0 ((uintptr_t)ep147_addr[EPI_1fa16d0])
#define EP_22043f0 ((uintptr_t)ep147_addr[EPI_22043f0])
#define EP_25f9c00 ((uintptr_t)ep147_addr[EPI_25f9c00])
#define EP_2754518 ((uintptr_t)ep147_addr[EPI_2754518])
#define EP_2798328 ((uintptr_t)ep147_addr[EPI_2798328])
#define EP_27b8a50 ((uintptr_t)ep147_addr[EPI_27b8a50])
#define EP_27ba108 ((uintptr_t)ep147_addr[EPI_27ba108])
#define EP_27bb148 ((uintptr_t)ep147_addr[EPI_27bb148])
#define EP_289f798 ((uintptr_t)ep147_addr[EPI_289f798])
#define EP_289f878 ((uintptr_t)ep147_addr[EPI_289f878])
#define EP_2dd9288 ((uintptr_t)ep147_addr[EPI_2dd9288])
#define EP_2ddb878 ((uintptr_t)ep147_addr[EPI_2ddb878])
#endif
