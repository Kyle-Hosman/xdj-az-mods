# SPDX-License-Identifier: GPL-3.0-only
# tools/fuzz/lib.sh: sourced by tools/fuzz/run.sh and tools/unit-tests/run.sh.
#
# fz_prepare <name> <dir>   copy or extract the real source a harness needs into <dir>, the way
#                           tools/unit-tests/run.sh does for its suites (never a stale copy);
#                           a control then mutates a file in <dir> before fz_compile
# fz_compile <name> <dir> <out> <fuzz|replay>
#                           fuzz: clang-10 -fsanitize=fuzzer,address,undefined (libFuzzer's main)
#                           replay: $FZ_CC (default cc) -DFZ_REPLAY, with ASan+UBSan when that
#                           compiler has them: runs the inputs named on its command line once
# Must be sourced from the repository root.
#
# The harnesses (tools/fuzz/<name>_fuzz.c), each over input that can come from outside:
#   azconf    azmod.conf load/save and the azmod.cfg migration (guest/azmod/azconf.c)
#   cmjson    a community mod's azmod.json, its min_core version, the approved list of id +
#             sha256 pins (guest/azmod/community.c)
#   cmcat     the fetcher's catalog and status files, through the Mod Menu's layout
#             (guest/azmod/screen_community.c, via menu.c)
#   png       the community icon PNG gate (guest/azmod/community_icons.c cmi_png_ok)
#   bus       the panel-bus read() path over arbitrary frames and read sizes
#             (guest/azmod/bus_interpose.c)
#   drumwav   Drum Roll WAV samples and file names (guest/azstem/drumroll.c)
#   stemname  STEMS folder names, stem file names, source.json (guest/azstem/stem_playback.c)
#   ofscache  the stem offset cache line, azstem.ofs (guest/azstem/ofsload.c)
#   sdklib    the SDK's library rows from a fake SQLite (NULLs, long text, errors, the row cap),
#             /proc/mounts and sysfs paths (guest/azmod/sdk_lib.c)
#   pdbsync   a destination and a source export.pdb (guest/azmod/pdb_sync.c)
#   elfcheck  a community mod's .so before approval (guest/azmod/community.c cm_elf_check)
#   mp4       MP3 / M4A source tracks: ID3 + Info/Xing, the MP4 box walk (guest/azstem/ofsload.c)
#   hcanlz    a track's analysis file, for its hot cues (guest/azmod/hc_anlz.h)
#   clocktz   a time zone file for the Clock readout (guest/azmod/clock_tz.h)
#   wzsave    Remember Waveform Zoom's saved value, azmod.conf's wave.zoom (guest/azmod/wave_zoom.h)
#   inputmem  the deck's settings file, for Channel Input Select Memory (guest/azmod/input_memory.h)
#   cmbrowse  Community Mods > Browse: a mod's category and updated values and the
#             orders over them (guest/azmod/cm_browse.h)
#   fcopy     folder and file names from a stick, for the FOLDER-view copy plan (folder_copy.h)
#   colwidths Column Edit's saved column widths (guest/azmod/browse_edit.h)
#   colfields Column Edit's saved column fields, and a library text as a field shows it
#             (guest/azmod/browse_fields.h)
FZ_NAMES="azconf cmjson cmcat png bus drumwav stemname ofscache sdklib pdbsync elfcheck mp4 hcanlz clocktz cmbrowse inputmem fcopy colwidths colfields wzsave"
FZ_T=tools/fuzz
FZ_UT=tools/unit-tests

# the azmod.json scanner and the small validators of community.c: one region, statics
fz_cmjson_extract() {
    awk '/^static const char \*cm_json_ws\(/ {p=1} p {print} p && /^static int cm_json_fw\(/ {q=1} q && /^}$/ {exit}' guest/azmod/community.c
    awk '/^static int cm_ver3\(/ {p=1} p {print} p && /^static int cm_vercmp\(/ {q=1} q && /^}$/ {exit}' guest/azmod/community.c
    awk '/^static int cm_id_ok\(const char \*id\) \{/ {p=1} p {print} p && /^}$/ {exit}' guest/azmod/community.c
    # the approved list (id + sha256 pins of mods the user approved): read, put, look up
    perl -0ne 'print $1 if /(static struct \{\n    char id\[33\], sha\[65\];\n\} g_cm_ok.*?\nstatic int cm_is_approved\(.*?\n\}\n)/s' guest/azmod/community.c
}

fz_prepare() {
    local name=$1 dir=$2
    mkdir -p "$dir"
    case $name in
    azconf)
        cp guest/azmod/azconf.c guest/azmod/azconf.h "$dir/" ;;
    bus)
        mkdir -p "$dir/azmod"
        cp guest/azmod/*.h "$dir/azmod/"
        awk '/^\/\* ---- read\(\): the panel input buses/ {p=1} p' guest/azmod/bus_interpose.c > "$dir/read_path.inc"
        awk '/^static uint16_t crc16_x25\(/ {p=1} /^static void bus_fd_drop\(/ {p=0} p' guest/azmod/bus_interpose.c > "$dir/crc.inc"
        cp $FZ_UT/busread_trace_test.c "$dir/"
        [ -s "$dir/read_path.inc" ] && grep -q 'crc16_x25' "$dir/crc.inc" ;;
    cmjson)
        fz_cmjson_extract > "$dir/cmjson.inc"
        grep -q 'cm_json_walk' "$dir/cmjson.inc" && grep -q 'cm_vercmp' "$dir/cmjson.inc" &&
            grep -q 'cm_id_ok' "$dir/cmjson.inc" && grep -q 'cm_sha_ok' "$dir/cmjson.inc" &&
            grep -q 'static void cm_approved_read' "$dir/cmjson.inc" && grep -q 'cm_is_approved' "$dir/cmjson.inc" ;;
    png)
        awk '/^static uint32_t cmi_crc/ {p=1} p {print} p && /no IEND/ {q=1} q && /^}$/ {exit}' guest/azmod/community_icons.c > "$dir/png.inc"
        grep -q 'static int cmi_png_fit(unsigned char \*b, int n, uint32_t maxw, uint32_t maxh) {' "$dir/png.inc" &&
            grep -q 'int cmi_png_ok' "$dir/png.inc" && grep -q 'no IEND' "$dir/png.inc" ;;
    pdbsync)
        cp guest/azmod/pdb_sync.c "$dir/" ;;
    clocktz)
        cp guest/azmod/clock_tz.h "$dir/" ;;
    wzsave)
        cp guest/azmod/wave_zoom.h "$dir/"
        grep -q 'static inline uint32_t wz_parse' "$dir/wave_zoom.h" ;;
    inputmem)
        cp guest/azmod/input_memory.h "$dir/" ;;
    colwidths)
        cp guest/azmod/browse_edit.h "$dir/" ;;
    colfields)
        cp guest/azmod/browse_fields.h "$dir/"
        grep -q 'static int b2_fields_parse' "$dir/browse_fields.h" ;;
    fcopy)
        cp guest/azmod/folder_copy.h "$dir/"
        grep -q 'static inline int fc_plan_make' "$dir/folder_copy.h" ;;
    cmbrowse)
        cp guest/azmod/cm_browse.h "$dir/"
        grep -q 'static inline int cmb_view' "$dir/cm_browse.h" ;;
    elfcheck)
        awk '/^static const char \*const CM_FORBIDDEN\[\]/ {p=1} p {print} p && /^static int cm_elf_check\(/ {q=1} q && /^}$/ {exit}' guest/azmod/community.c > "$dir/elfcheck.inc"
        cp guest/common/azsha256.h "$dir/"
        grep -q 'CM_LIBS' "$dir/elfcheck.inc" && grep -q 'azsha256_hex(b, sz, sha);' "$dir/elfcheck.inc" ;;
    stemname)
        { awk '/^static int slot_for_filename\(/ {p=1} p {print} p && /^}$/ {exit}' guest/azstem/stem_playback.c
          awk '/^static int stem_folder_rank\(/ {p=1} p {print} p && /^static int stem_folder_identity\(/ {q=1} q && /^}$/ {exit}' guest/azstem/stem_playback.c
        } > "$dir/stemname.inc"
        grep -q 'static int json_str' "$dir/stemname.inc" && grep -q 'file_fingerprint(src)' "$dir/stemname.inc" &&
            grep -q 'strcasestr(name, "vocal")' "$dir/stemname.inc" ;;
    ofscache)
        awk '/^#define OFSC_MAGIC / {p=1} p {print} p && /^static int ofs_cache_parse\(/ {q=1} q && /^}$/ {exit}' guest/azstem/ofsload.c > "$dir/ofscache.inc"
        grep -q 'static int ofsc_field' "$dir/ofscache.inc" && grep -q 'k != skey' "$dir/ofscache.inc" ;;
    drumwav)
        { awk '/^#define DRUM_MAX_SECONDS / {p=1} p {print} p && /^static float \*drum_decode_wav\(/ {q=1} q && /^}$/ {exit}' guest/azstem/drumroll.c
          awk '/^static int drum_slot_for_name\(/ {p=1} p {print} p && /^}$/ {exit}' guest/azstem/drumroll.c
        } > "$dir/drumwav.inc"
        cp guest/azstem/ep147rate.h "$dir/"
        grep -q 'DRUM_WAV_MAX_CHANS' "$dir/drumwav.inc" && grep -q 'drum_wav_format_ok(fmt, want' "$dir/drumwav.inc" &&
            grep -q '\*out_frames = out;' "$dir/drumwav.inc" && grep -q 'strstr(flat, "kick")' "$dir/drumwav.inc" ;;
    cmcat)
        mkdir -p "$dir/azmod" "$dir/common"
        cp guest/azmod/*.c guest/azmod/*.h guest/azmod/*.def "$dir/azmod/"
        cp guest/common/*.h "$dir/common/"
        [ ! -d guest/azmod/icons ] || cp -R guest/azmod/icons "$dir/azmod/"
        grep -q '^static void cm_load(void)' "$dir/azmod/screen_community.c" ;;
    sdklib)
        cp guest/azmod/sdk_lib.c guest/azmod/azmod-mod.h $FZ_UT/sdklib_test.c $FZ_UT/check.h "$dir/"
        grep -q '^static void sdk_copy_col' "$dir/sdk_lib.c" ;;
    hcanlz)
        cp guest/azmod/hc_anlz.h "$dir/"
        grep -q 'static inline int hc_anlz_hot_cues' "$dir/hc_anlz.h" ;;
    mp4)
        awk '/^static long ofs_stems_decoded\(/ {exit} {print}' guest/azstem/ofsload.c > "$dir/mp4.inc"
        cp guest/azstem/ep147rate.h "$dir/"
        grep -q '^static int mp3_probe(' "$dir/mp4.inc" && grep -q '^static float \*m4a_decode_native(' "$dir/mp4.inc" &&
            grep -q '^static int esds_asc(' "$dir/mp4.inc" ;;
    *)
        echo "fz_prepare: unknown harness $name" >&2; return 2 ;;
    esac || { echo "fz_prepare $name: extraction failed (did the source move?)" >&2; return 2; }
}

fz_cflags() {   # <name> <dir>: per-harness compile arguments (sources and includes)
    local name=$1 dir=$2
    FZ_OBJ_SRC=; FZ_OBJ_ARGS=()   # an extra translation unit with its own defines, if any
    case $name in
    azconf) FZ_ARGS=(-I"$dir" "$dir/azconf.c" $FZ_T/azconf_fuzz.c) ;;
    bus)    FZ_ARGS=(-I"$dir/azmod" -I"$dir" -I$FZ_T -DBI_INC='"read_path.inc"' -DCRC_INC='"crc.inc"'
                     -Wno-unused-parameter $FZ_T/bus_fuzz.c) ;;
    png)    FZ_ARGS=(-DPNG_INC="\"$dir/png.inc\"" $FZ_T/png_fuzz.c) ;;
    pdbsync) FZ_ARGS=(-I"$dir" $FZ_T/pdbsync_fuzz.c) ;;
    clocktz) FZ_ARGS=(-I"$dir" $FZ_T/clocktz_fuzz.c) ;;
    wzsave) FZ_ARGS=(-I"$dir" $FZ_T/wzsave_fuzz.c) ;;
    inputmem) FZ_ARGS=(-I"$dir" $FZ_T/inputmem_fuzz.c) ;;
    colwidths) FZ_ARGS=(-I"$dir" $FZ_T/colwidths_fuzz.c) ;;
    colfields) FZ_ARGS=(-I"$dir" $FZ_T/colfields_fuzz.c) ;;
    fcopy) FZ_ARGS=(-I"$dir" $FZ_T/fcopy_fuzz.c) ;;
    cmbrowse) FZ_ARGS=(-I"$dir" $FZ_T/cmbrowse_fuzz.c) ;;
    elfcheck) FZ_ARGS=(-I"$dir" -DELFCHECK_INC="\"$dir/elfcheck.inc\"" $FZ_T/elfcheck_fuzz.c) ;;
    stemname) FZ_ARGS=(-DSTEMNAME_INC="\"$dir/stemname.inc\"" $FZ_T/stemname_fuzz.c) ;;
    ofscache) FZ_ARGS=(-DOFSCACHE_INC="\"$dir/ofscache.inc\"" $FZ_T/ofscache_fuzz.c) ;;
    drumwav) FZ_ARGS=(-I"$dir" -DDRUMWAV_INC="\"$dir/drumwav.inc\"" $FZ_T/drumwav_fuzz.c) ;;
    cmcat)  FZ_ARGS=(-I"$dir/azmod" -DMENU_C="\"$dir/azmod/menu.c\"" $FZ_T/cmcat_fuzz.c)
            # the rest of azmod: lanetest.c's stand-ins, its own object (as the unit suites build it)
            FZ_OBJ_SRC="$dir/azmod/lanetest.c"
            FZ_OBJ_ARGS=(-I"$dir/azmod" -Dmain=lanetest_main -Dazmod_ttf_menu=lanetest_ttf_menu
                         -Dazmod_ttf_menu2=lanetest_ttf_menu2) ;;
    sdklib) FZ_ARGS=(-I"$dir" -DLIB_INC="\"$dir/sdk_lib.c\"" $FZ_T/sdklib_fuzz.c -lsqlite3 -ldl) ;;
    mp4)    FZ_ARGS=(-I"$dir" -DMP4_INC="\"$dir/mp4.inc\"" $FZ_T/mp4_fuzz.c -ldl) ;;
    hcanlz) FZ_ARGS=(-DHCANLZ_HDR="\"$dir/hc_anlz.h\"" $FZ_T/hcanlz_fuzz.c) ;;
    cmjson) FZ_ARGS=(-I"$dir" -DCMJSON_INC="\"$dir/cmjson.inc\"" $FZ_T/cmjson_fuzz.c) ;;
    esac
}

fz_compile() {
    local name=$1 dir=$2 out=$3 mode=$4 log=$3.log
    fz_cflags "$name" "$dir"
    # -fno-builtin: memcpy/strlen go through the sanitizer runtime, which checks the whole range
    # (an inlined 4-byte copy that straddles the end of a buffer is otherwise missed)
    local common=(-std=gnu11 -g -O1 -w -fno-builtin -I$FZ_T -D_GNU_SOURCE)
    local cc san
    if [ "$mode" = fuzz ]; then
        cc=clang-10
        san=(-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)
    else
        cc=${FZ_CC:-cc}
        san=(-fno-inline -fno-omit-frame-pointer -DFZ_REPLAY -fsanitize=address,undefined -fno-sanitize-recover=undefined)
        # a compiler without the sanitizers still replays (plain build)
        echo 'int main(void){return 0;}' > "$out.probe.c"
        $cc "${san[@]}" -o "$out.probe" "$out.probe.c" >/dev/null 2>&1 || san=(-DFZ_REPLAY)
    fi
    local objs=()
    if [ -n "$FZ_OBJ_SRC" ]; then
        local osan=("${san[@]/-fsanitize=fuzzer,/-fsanitize=fuzzer-no-link,}")
        $cc "${common[@]}" "${osan[@]}" ${FZ_OBJ_ARGS[@]+"${FZ_OBJ_ARGS[@]}"} -c -o "$out.extra.o" "$FZ_OBJ_SRC" >"$log" 2>&1 ||
            { grep -v 'unsupported GNU_PROPERTY_TYPE' "$log" | tail -20; return 1; }
        objs=("$out.extra.o")
    fi
    $cc "${common[@]}" "${san[@]}" -o "$out" "${FZ_ARGS[@]}" ${objs[@]+"${objs[@]}"} -lpthread -lm >"$log" 2>&1 ||
        { grep -v 'unsupported GNU_PROPERTY_TYPE' "$log" | tail -20; return 1; }
}

# fz_has_libfuzzer: clang-10 with the libFuzzer runtime (docker/azmod-test.Dockerfile)
fz_has_libfuzzer() {
    command -v clang-10 >/dev/null 2>&1 &&
        [ -n "$(ls /usr/lib/llvm-10/lib/clang/10.0.0/lib/linux/libclang_rt.fuzzer-*.a 2>/dev/null)" ]
}
