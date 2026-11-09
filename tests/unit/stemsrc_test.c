/* SPDX-License-Identifier: GPL-3.0-only */
/* Is the file at a stems folder's recorded path still its song (guest/azstem/stem_source_rule.h):
 * tag and art edits are, a much different file is not, in both directions and at the limit.
 * STEMSRC_HDR = the header under test. */
#include "check.h"
#include STEMSRC_HDR

int main(void) {
    CHECK("same size", stem_same_path_same_file(9002974, 9002974));
    CHECK("a tag edit that grew the file", stem_same_path_same_file(9002974, 9007113));
    CHECK("a tag edit that shrank the file", stem_same_path_same_file(9007113, 9002974));
    CHECK("exactly at the limit", stem_same_path_same_file(1000, 1000 + STEM_SAME_PATH_SLACK));
    CHECK("just past the limit", !stem_same_path_same_file(1000, 1001 + STEM_SAME_PATH_SLACK));
    CHECK("past the limit, smaller", !stem_same_path_same_file(1001 + STEM_SAME_PATH_SLACK, 1000));
    return ut_summary();
}
