/* SPDX-License-Identifier: GPL-3.0-only */
/* Is a file at the path a stems folder records still the file those stems came from?
 * Editing tags or cover art rewrites the file and changes its size and fingerprint, but not its
 * audio. Below this limit a size change at the same path counts as such an edit. Must match
 * AZ-MODS library.SAME_PATH_SLACK. */
#ifndef STEM_SOURCE_RULE_H
#define STEM_SOURCE_RULE_H

#define STEM_SAME_PATH_SLACK (1LL << 20)

static inline int stem_same_path_same_file(long long recorded, long long now) {
    long long d = recorded > now ? recorded - now : now - recorded;
    return d <= STEM_SAME_PATH_SLACK;
}
#endif
