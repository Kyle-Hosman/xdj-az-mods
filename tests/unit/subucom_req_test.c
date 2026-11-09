/* SPDX-License-Identifier: GPL-3.0-only */
/* subucom_msg_req (extracted from src/azmod/bus_interpose.c by run.sh): only the SUBUCOM message
 * request, whose argument is the 16-byte {u32, u32 size, u64 frame} descriptor, may have its
 * descriptor fields read. The other type-0x70 requests the deck sends carry 1-, 2- or 4-byte
 * arguments. */
#include <stdio.h>
#include SUBUCOM_INC
#include "check.h"

int main(void) {
    ut_suite = "subucom_msg_req";
    CHECK("0x40107000 (message, 16-byte descriptor) is a message", subucom_msg_req(0x40107000ul));
    static const unsigned long other[] = {
        0x80027003ul, /* bits per word, 2 bytes */
        0x40027003ul,
        0x80017001ul, /* timer status, 1 byte */
        0x40017001ul,
        0x80047002ul, /* rx bytes, 4 bytes */
        0x800170c0ul,
        0x40106b00ul, /* 16 bytes, another type */
        0x40207000ul, /* type 0x70, 32 bytes */
    };
    char name[96];
    for (unsigned i = 0; i < sizeof other / sizeof other[0]; i++) {
        snprintf(name, sizeof name, "%#lx is not a message", other[i]);
        CHECK(name, !subucom_msg_req(other[i]));
    }
    return ut_summary();
}
