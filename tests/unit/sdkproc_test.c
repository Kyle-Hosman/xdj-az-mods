/* SPDX-License-Identifier: GPL-3.0-only */
/* The "other programs" services for community mods (guest/azmod/sdk_proc.c): launch,
 * launch_alive, launch_stop, handoff's guards, reload. The real file is built here against
 * stand-ins for the community host (one table of mods, each with an assets and a data folder in
 * a scratch directory). Questions asked:
 *   - only the mod's own executable files may run: a path that leaves its folders by "..", a
 *     link out of them, a file that is not executable, a relative path: all refused;
 *   - the program starts without the mods' LD_PRELOAD, and without EP147's descriptors that are
 *     not close-on-exec (a stick's file, a bus device);
 *   - a mod can ask about and stop only programs it launched itself;
 *   - launch runs it in a systemd scope of its own, with the nice level, CPUs and memory limit;
 *   - its log is capped and rotated once (the cap is made small here);
 *   - handoff never starts without a way to ask the user, and is refused while a deck plays,
 *     even after the user said yes; when it runs, EP147 comes back however the program ends.
 * The controls take one guard out each; each must fail. PROC_INC = the sdk_proc.c under test.
 * Linux only (posix_spawn scheduling attributes). */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "check.h"
#include "azmod-mod.h"

extern char **environ;

/* ---- the community host, as sdk_proc.c sees it ---- */
#define AZMOD_STEM_DECKS 4
typedef struct {
    char id[33], name[48];
    int sticky;
} cm_mod;
static cm_mod g_cm[2] = {{"mod-a", "Mod A"}, {"mod-b", "Mod B"}};
static char g_dir[2][2][PATH_MAX]; /* [mod][0 assets, 1 data] */
static pthread_mutex_t g_cm_mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static __thread int g_cm_in_frame;
static char g_last_log[512];
static cm_mod *cm_find(const char *id) {
    for (int i = 0; i < 2; i++)
        if (id && !strcmp(g_cm[i].id, id))
            return &g_cm[i];
    return NULL;
}
static cm_mod *cm_self(const char *id, uintptr_t ra, const char *what) {
    (void) ra, (void) what;
    return g_cm_in_frame ? NULL : cm_find(id);
}
#define CM_RA() ((uintptr_t) 0)
static void cm_logf(cm_mod *c, int force, const char *fmt, ...) {
    (void) force;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_log, sizeof g_last_log, fmt, ap);
    va_end(ap);
    (void) c;
}
static const char *cm_assets_dir_u(const char *id) {
    cm_mod *c = cm_find(id);
    return c ? g_dir[c - g_cm][0] : NULL;
}
static const char *cm_data_dir_u(const char *id) {
    cm_mod *c = cm_find(id);
    return c ? g_dir[c - g_cm][1] : NULL;
}
static int az_thread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    return pthread_create(t, a, fn, arg);
}
static int g_any_loaded;
static int azcm_any_loaded(void) { return g_any_loaded; }
static void azcm_reload_requested(void) {}
static int g_sdk_reload_req;

/* azstem's playhead, looked up with dlsym: a playing deck's moves. */
static _Atomic int g_playing;
static _Atomic long long g_pos;
long long azstem_play_obs(int deck);
long long azstem_play_obs(int deck) {
    if (deck != 1)
        return -1; /* only deck 2 has a track */
    return atomic_load(&g_playing) ? atomic_fetch_add(&g_pos, 4410) : 1000;
}

/* systemd-run is a stand-in script in the scratch folder: it records its arguments, and for a
 * scope (launch) execs the command after "--" as the real one does; a handoff is only recorded,
 * so a test can never stop anything on the machine running it. */
static char g_fake_sdrun[PATH_MAX], g_sdrun_args[PATH_MAX];
#define SDK_SYSTEMD_RUN g_fake_sdrun
static char g_guard[PATH_MAX]; /* the handoff guard as the stage holds it (absent until a case puts it) */
#define SDK_GUARD_STAGE g_guard
static char g_guard_run[PATH_MAX]; /* where the core copies it to run it (/run on the deck) */
#define SDK_GUARD_RUN g_guard_run
#define SDK_LOG_CAP (64 * 1024) /* the cap, small enough to pass quickly */
static int (*g_sdk_confirm_core)(const char *title, const char *text, const char *yes,
                                 void (*fn)(int yes, void *user), void *user);

#include PROC_INC

/* ---- fixture ---- */
static char g_root[PATH_MAX];
static void put_file(const char *path, const char *body, int mode) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(body, f);
        fclose(f);
    }
    chmod(path, mode);
}
static int count_fds(void) { /* this process's open descriptors */
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    while (d && readdir(d))
        n++;
    if (d)
        closedir(d);
    return n;
}
static int recorded(const char *arg) { /* 1 = systemd-run was given exactly this argument */
    FILE *f = fopen(g_sdrun_args, "r");
    char line[PATH_MAX + 64];
    int hit = 0;
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        hit |= !strcmp(line, arg);
    }
    if (f)
        fclose(f);
    return hit;
}
static int handoff_done(void) { /* the handoff thread has finished (or never started) */
    for (int i = 0; i < 100 && atomic_load(&g_sdk_handoff_busy); i++)
        usleep(50000);
    return !atomic_load(&g_sdk_handoff_busy);
}
static int g_confirm_calls, g_confirm_answer, g_confirm_busy;
static int stub_confirm(const char *title, const char *text, const char *yes,
                        void (*fn)(int yes, void *user), void *user) {
    (void) title, (void) text, (void) yes;
    g_confirm_calls++;
    if (g_confirm_busy)
        return -1;
    fn(g_confirm_answer, user);
    return 0;
}
static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long) st.st_size : -1;
}

static int hard_link(const char *from, const char *to) { return link(from, to); }

int main(void) {
    ut_suite = "sdk: other programs";
    snprintf(g_root, sizeof g_root, "%s/.az-sdkproc.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(g_root))
        return 2;
    char p[PATH_MAX], out[PATH_MAX];
    for (int m = 0; m < 2; m++)
        for (int k = 0; k < 2; k++) {
            snprintf(g_dir[m][k], PATH_MAX, "%s/%s-%s", g_root, g_cm[m].id, k ? "data" : "assets");
            mkdir(g_dir[m][k], 0755);
        }
    char outside[PATH_MAX];
    snprintf(outside, sizeof outside, "%s/outside", g_root);
    mkdir(outside, 0755);
    snprintf(p, sizeof p, "%s/tool", outside);
    put_file(p, "#!/bin/sh\nexit 0\n", 0755);
    snprintf(p, sizeof p, "%s/tool", g_dir[0][0]);
    put_file(p, "#!/bin/sh\nexit 0\n", 0755);
    snprintf(p, sizeof p, "%s/plain.txt", g_dir[0][0]);
    put_file(p, "not a program\n", 0644);
    snprintf(p, sizeof p, "%s/kit", g_dir[0][1]);
    put_file(p, "#!/bin/sh\nexit 0\n", 0755);
    snprintf(p, sizeof p, "%s/sleeper", g_dir[0][0]);
    put_file(p, "#!/bin/sh\nexec sleep 30\n", 0755);
    snprintf(g_sdrun_args, sizeof g_sdrun_args, "%s/systemd-run.args", g_root);
    snprintf(g_fake_sdrun, sizeof g_fake_sdrun, "%s/systemd-run", g_root);
    snprintf(g_guard, sizeof g_guard, "%s/az-handoff-guard", g_root);
    snprintf(g_guard_run, sizeof g_guard_run, "%s/run-az-handoff-guard", g_root);
    char script[2 * PATH_MAX];
    snprintf(script, sizeof script,
             "#!/bin/sh\nfor a in \"$@\"; do echo \"$a\"; done > '%s'\n"
             "case \"$1\" in --scope) ;; *) exit 0 ;; esac\n"
             "while [ \"$1\" != -- ]; do shift; done; shift; exec \"$@\"\n",
             g_sdrun_args);
    put_file(g_fake_sdrun, script, 0755);
    char link[PATH_MAX];
    snprintf(link, sizeof link, "%s/escape", g_dir[0][0]);
    snprintf(p, sizeof p, "%s/tool", outside);
    if (symlink(p, link) != 0)
        return 2;

    /* 1. the mod's own executable files only */
    snprintf(p, sizeof p, "%s/tool", g_dir[0][0]);
    CHECK("a program in the mod's assets folder may run", sdk_own_program("mod-a", p, out, sizeof out));
    snprintf(p, sizeof p, "%s/kit", g_dir[0][1]);
    CHECK("a program in the mod's data folder may run", sdk_own_program("mod-a", p, out, sizeof out));
    snprintf(p, sizeof p, "%s/../outside/tool", g_dir[0][0]);
    CHECK("a path that leaves the folder by .. is refused", !sdk_own_program("mod-a", p, out, sizeof out));
    CHECK("a link that points out of the folder is refused", !sdk_own_program("mod-a", link, out, sizeof out));
    snprintf(p, sizeof p, "%s/plain.txt", g_dir[0][0]);
    CHECK("a file that is not executable is refused", !sdk_own_program("mod-a", p, out, sizeof out));
    CHECK("a relative path is refused", !sdk_own_program("mod-a", "tool", out, sizeof out));
    snprintf(p, sizeof p, "%s/tool", g_dir[0][0]);
    CHECK("another mod's program is refused", !sdk_own_program("mod-b", p, out, sizeof out));
    snprintf(p, sizeof p, "%s/outside/tool", g_root);
    CHECK("launch refuses a program outside the mod's folders", sdk_launch("mod-a", p, NULL, NULL) == -1);
    az_launch_opts o = {sizeof o, 0, 0, 0, "/tmp/elsewhere.log"};
    snprintf(p, sizeof p, "%s/tool", g_dir[0][0]);
    CHECK("launch refuses a log outside the mod's data folder", sdk_launch("mod-a", p, NULL, &o) == -1);
    {
        /* the log's own name: the folder is resolved, so the last part must not lead out either */
        char victim[PATH_MAX], lnk[PATH_MAX], hard[PATH_MAX], name[PATH_MAX], lo[PATH_MAX];
        snprintf(victim, sizeof victim, "%s/outside/victim.log", g_root);
        FILE *vf = fopen(victim, "w");
        if (vf)
            fclose(vf);
        snprintf(lnk, sizeof lnk, "%s/linked.log", g_dir[0][1]);
        unlink(lnk);
        int made = symlink(victim, lnk) == 0;
        CHECK("a log that is a link out of the data folder is refused", made && !sdk_own_log("mod-a", lnk, lo, sizeof lo));
        off_t sz = 0;
        int fd = sdk_log_open(lnk, &sz);
        CHECK("... and the log is never opened through a link", made && fd < 0);
        if (fd >= 0)
            close(fd);
        snprintf(hard, sizeof hard, "%s/hard.log", g_dir[0][1]);
        unlink(hard);
        made = hard_link(victim, hard) == 0;
        CHECK("a log that is a second name of a file elsewhere (hard link) is refused",
              made && !sdk_own_log("mod-a", hard, lo, sizeof lo));
        snprintf(name, sizeof name, "%s/..", g_dir[0][1]);
        CHECK("a log named .. is refused", !sdk_own_log("mod-a", name, lo, sizeof lo));
        snprintf(name, sizeof name, "%s/.", g_dir[0][1]);
        CHECK("a log named . is refused", !sdk_own_log("mod-a", name, lo, sizeof lo));
        snprintf(name, sizeof name, "%s/fresh.log", g_dir[0][1]);
        CHECK("a new log in the data folder is taken (CONTROL)", sdk_own_log("mod-a", name, lo, sizeof lo));
        o.log_path = lnk;
        CHECK("launch refuses a log that is a link out", sdk_launch("mod-a", p, NULL, &o) == -1);
        o.log_path = "/tmp/elsewhere.log";
    }
    g_cm_in_frame = 1;
    CHECK("launch is refused from the control-bus thread", sdk_launch("mod-a", p, NULL, NULL) == -1);
    g_cm_in_frame = 0;

    /* 2. no mods preload in the child */
    setenv("LD_PRELOAD", "/nonexistent/azmod.so", 1);
    char logf[PATH_MAX];
    snprintf(logf, sizeof logf, "%s/env.log", g_dir[0][1]);
    char *const envv[] = {"/usr/bin/env", NULL};
    pid_t pid = sdk_spawn_clean(envv, logf);
    int st = 0;
    if (pid > 0)
        waitpid(pid, &st, 0);
    unsetenv("LD_PRELOAD");
    for (int i = 0; i < 40 && file_size(logf) <= 0; i++) /* the copier writes it after the exit */
        usleep(50000);
    usleep(100000);
    FILE *f = fopen(logf, "r");
    int has_path = 0, has_preload = 0;
    char line[4096];
    while (f && fgets(line, sizeof line, f)) {
        has_path |= !strncmp(line, "PATH=", 5);
        has_preload |= !strncmp(line, "LD_PRELOAD=", 11);
    }
    if (f)
        fclose(f);
    CHECK("the program's environment arrived (its output reached the log)", pid > 0 && has_path);
    CHECK("the program starts without the mods' LD_PRELOAD", pid > 0 && !has_preload);

    /* 2a. no inherited descriptors: one EP147 opened without O_CLOEXEC, at a known number */
    int leak = open("/dev/null", O_RDONLY); /* no O_CLOEXEC, as EP147's own */
    int fdno = leak >= 0 ? dup2(leak, 77) : -1;
    char fdlog[PATH_MAX];
    snprintf(fdlog, sizeof fdlog, "%s/fd.log", g_dir[0][1]);
    char *const fdv[] = {"/bin/sh", "-c", "if [ -e /proc/self/fd/77 ]; then echo HELD; else echo CLEAN; fi", NULL};
    pid = fdno == 77 ? sdk_spawn_clean(fdv, fdlog) : -1;
    if (pid > 0)
        waitpid(pid, &st, 0);
    for (int i = 0; i < 40 && file_size(fdlog) <= 0; i++)
        usleep(50000);
    usleep(100000);
    char fdres[32] = "";
    f = fopen(fdlog, "r");
    if (f && !fgets(fdres, sizeof fdres, f))
        fdres[0] = 0;
    if (f)
        fclose(f);
    CHECK("the descriptor is open in EP147 (the precondition)", fdno == 77 && fcntl(77, F_GETFD) >= 0);
    CHECK("the program does not inherit EP147's descriptors", pid > 0 && !strncmp(fdres, "CLEAN", 5));
    if (leak >= 0)
        close(leak);
    if (fdno == 77)
        close(77);

    /* 2b. the log is capped and rotated once: 2.5 caps written, at most a cap kept in each */
    char bigp[PATH_MAX], biglog[PATH_MAX], old1[PATH_MAX + 4];
    snprintf(bigp, sizeof bigp, "%s/chatty", g_dir[0][0]);
    put_file(bigp, "#!/bin/sh\ni=0\nwhile [ $i -lt 2560 ]; do printf '%063d\\n' $i; i=$((i+1)); done\n", 0755);
    snprintf(biglog, sizeof biglog, "%s/chatty.log", g_dir[0][1]);
    snprintf(old1, sizeof old1, "%s.1", biglog);
    char *const bigv[] = {bigp, NULL};
    pid = sdk_spawn_clean(bigv, biglog);
    if (pid > 0)
        waitpid(pid, &st, 0);
    long cur = -1, prev = -1;
    for (int i = 0; i < 60; i++) { /* until the copier has drained the pipe: sizes stop moving */
        usleep(50000);
        long c2 = file_size(biglog), p2 = file_size(old1);
        if (c2 == cur && p2 == prev && c2 >= 0)
            break;
        cur = c2, prev = p2;
    }
    char m[160];
    snprintf(m, sizeof m, "a program's log stays under the cap (%ld bytes, cap %d)", cur, SDK_LOG_CAP);
    CHECK(m, cur > 0 && cur <= SDK_LOG_CAP);
    snprintf(m, sizeof m, "and is rotated once to .1 (%ld bytes)", prev);
    CHECK(m, prev > 0 && prev <= SDK_LOG_CAP && access(old1, F_OK) == 0);
    char old2[PATH_MAX + 4];
    snprintf(old2, sizeof old2, "%s.2", biglog);
    CHECK("only one old log is kept (no .2)", access(old2, F_OK) != 0);
    FILE *lf = fopen(biglog, "r");
    char last[128] = "", ln[128];
    while (lf && fgets(ln, sizeof ln, lf))
        snprintf(last, sizeof last, "%s", ln);
    if (lf)
        fclose(lf);
    CHECK("the newest output is the one kept", atoi(last) == 2559);

    /* 3. a mod asks about and stops only its own programs */
    char *const sleepv[] = {"/bin/sleep", "30", NULL};
    pid = sdk_spawn_clean(sleepv, NULL);
    struct sdk_proc *slot = &g_sdk_proc[0];
    snprintf(slot->id, sizeof slot->id, "mod-a");
    slot->pid = pid;
    atomic_store(&slot->running, 1);
    sdk_detach(sdk_proc_waiter, slot);
    CHECK("its own program: alive", sdk_launch_alive("mod-a", pid) == 1);
    CHECK("another mod asking about it: not reported", sdk_launch_alive("mod-b", pid) == 0);
    CHECK("another mod stopping it: refused", sdk_launch_stop("mod-b", pid) == -1);
    CHECK("sdk_proc_owns: the owner", sdk_proc_owns("mod-a", pid) == 1);
    CHECK("sdk_proc_owns: another mod", sdk_proc_owns("mod-b", pid) == 0);
    CHECK("its own program: stopped", sdk_launch_stop("mod-a", pid) == 0);
    for (int i = 0; i < 40 && sdk_launch_alive("mod-a", pid); i++)
        usleep(100000);
    CHECK("the stopped program has exited", sdk_launch_alive("mod-a", pid) == 0);

    /* 3b. launch, end to end: a scope of its own, nice and CPUs passed on, a pid the mod owns */
    snprintf(p, sizeof p, "%s/sleeper", g_dir[0][0]);
    char lp[PATH_MAX];
    snprintf(lp, sizeof lp, "%s/run.log", g_dir[0][1]);
    az_launch_opts lo = {sizeof lo, 5, 0x3, 64, lp};
    const char *largs[] = {"--flag", NULL};
    unlink(g_sdrun_args);
    pid = sdk_launch("mod-a", p, largs, &lo);
    for (int i = 0; i < 40 && !recorded(p); i++) /* the stand-in writes its record as it starts */
        usleep(50000);
    CHECK("launch runs the mod's program and returns its pid", pid > 0 && sdk_launch_alive("mod-a", pid) == 1);
    CHECK("in a systemd scope of its own, with its memory limit", recorded("--scope") &&
                                                                      recorded("--property=MemoryLimit=64M"));
    CHECK("its nice level and CPU mask are passed on", recorded("/usr/bin/nice") && recorded("5") &&
                                                           recorded("/usr/bin/taskset") && recorded("3"));
    CHECK("the program and its arguments, as given", recorded(p) && recorded("--flag"));
    CHECK("another mod cannot stop it", sdk_launch_stop("mod-b", pid) == -1 && sdk_launch_alive("mod-a", pid));
    sdk_launch_stop("mod-a", pid);
    for (int i = 0; i < 40 && sdk_launch_alive("mod-a", pid); i++)
        usleep(100000);
    CHECK("and its owner can", sdk_launch_alive("mod-a", pid) == 0);

    /* 3c. the limits: arguments, program slots; a spawn that fails leaves nothing open */
    const char *many[SDK_ARGV_MAX + 2];
    for (int i = 0; i <= SDK_ARGV_MAX; i++)
        many[i] = "x";
    many[SDK_ARGV_MAX + 1] = NULL;
    CHECK("launch with more than 32 arguments: refused", sdk_launch("mod-a", p, many, NULL) == -1 &&
                                                             strstr(g_last_log, "more than 32 arguments"));
    many[SDK_ARGV_MAX] = NULL;
    pid = sdk_launch("mod-a", p, many, NULL);
    CHECK("... exactly 32 is taken (CONTROL)", pid > 0);
    sdk_launch_stop("mod-a", pid);
    for (int i = 0; i < 40 && sdk_launch_alive("mod-a", pid); i++)
        usleep(100000);
    int idle = 1;
    for (int i = 0; i < SDK_PROCS; i++)
        idle &= !atomic_load(&g_sdk_proc[i].running);
    for (int i = 0; i < SDK_PROCS; i++)
        atomic_store(&g_sdk_proc[i].running, 1); /* every slot taken */
    CHECK("every program slot in use: refused, nothing started",
          idle && sdk_launch("mod-a", p, NULL, NULL) == -1 && strstr(g_last_log, "programs are already running"));
    for (int i = 0; i < SDK_PROCS; i++)
        atomic_store(&g_sdk_proc[i].running, 0);
    int fds0 = count_fds();
    char nolog[PATH_MAX];
    snprintf(nolog, sizeof nolog, "%s/never.log", g_dir[0][1]);
    char *const nov[] = {"/nonexistent/program", NULL};
    pid_t np = sdk_spawn_clean(nov, nolog);
    for (int i = 0; i < 40 && count_fds() > fds0; i++) /* the log copier sees end of file and ends */
        usleep(50000);
    CHECK("a spawn that fails: -1, its pipe and log copier gone", np == -1 && count_fds() == fds0);

    /* 4. handoff: never without the user's yes, never while a deck plays */
    snprintf(p, sizeof p, "%s/tool", g_dir[0][0]);
    g_sdk_confirm_core = NULL;
    CHECK("handoff with no way to ask the user: refused", sdk_handoff("mod-a", p, NULL) == -1 &&
                                                              strstr(g_last_log, "no confirm dialog"));
    g_sdk_confirm_core = stub_confirm;
    g_confirm_busy = 1;
    CHECK("the dialog busy with another question: refused, and nothing left pending",
          sdk_handoff("mod-a", p, NULL) == -1 && atomic_load(&g_sdk_handoff_busy) == 0);
    g_confirm_busy = 0;
    g_confirm_calls = 0;
    g_confirm_answer = 0;
    CHECK("handoff asks the user first", sdk_handoff("mod-a", p, NULL) == 0 && g_confirm_calls == 1);
    CHECK("a No ends it (nothing left running)", atomic_load(&g_sdk_handoff_busy) == 0);
    snprintf(p, sizeof p, "%s/outside/tool", g_root);
    CHECK("handoff refuses a program outside the mod's folders", sdk_handoff("mod-a", p, NULL) == -1 &&
                                                                     g_confirm_calls == 1);
    snprintf(p, sizeof p, "%s/tool", g_dir[0][0]);
    {
        const char *ha[SDK_ARGV_MAX + 2];
        static char lng[SDK_ARGV_MAX][128];
        for (int i = 0; i < SDK_ARGV_MAX; i++) {
            memset(lng[i], 'a', 127);
            lng[i][127] = 0;
            ha[i] = lng[i];
        }
        ha[SDK_ARGV_MAX] = "one more";
        ha[SDK_ARGV_MAX + 1] = NULL;
        g_confirm_calls = 0;
        CHECK("handoff with more than 32 arguments: refused, nothing asked",
              sdk_handoff("mod-a", p, ha) == -1 && strstr(g_last_log, "too many or too long") && g_confirm_calls == 0);
        ha[SDK_ARGV_MAX] = NULL;
        CHECK("handoff with 32 arguments of 127 bytes: asks (CONTROL)", sdk_handoff("mod-a", p, ha) == 0 &&
                                                                            g_confirm_calls == 1 && handoff_done());
        char over[129];
        memset(over, 'b', 128);
        over[128] = 0;
        const char *ov[] = {over, NULL};
        CHECK("handoff with an argument of 128 bytes: refused", sdk_handoff("mod-a", p, ov) == -1 &&
                                                                    strstr(g_last_log, "too many or too long"));
        atomic_store(&g_sdk_handoff_busy, 1);
        g_confirm_calls = 0;
        CHECK("another handoff in progress: refused, nothing asked",
              sdk_handoff("mod-a", p, NULL) == -1 && strstr(g_last_log, "in progress") && g_confirm_calls == 0);
        atomic_store(&g_sdk_handoff_busy, 0);
    }
    atomic_store(&g_playing, 1);
    CHECK("a deck playing is seen as playing", sdk_any_deck_playing() == 1);
    atomic_store(&g_playing, 0);
    CHECK("stopped decks are seen as stopped", sdk_any_deck_playing() == 0);
    snprintf(p, sizeof p, "%s/tool", g_dir[0][0]);
    unlink(g_sdrun_args);
    atomic_store(&g_playing, 1);
    g_confirm_answer = 1;
    CHECK("handoff, Yes, while a deck plays: accepted for the check", sdk_handoff("mod-a", p, NULL) == 0);
    CHECK("... and refused: EP147 is never stopped", handoff_done() && access(g_sdrun_args, F_OK) != 0 &&
                                                         strstr(g_last_log, "a deck is playing"));
    atomic_store(&g_playing, 0);
    CHECK("handoff, Yes, decks stopped: started", sdk_handoff("mod-a", p, NULL) == 0 && handoff_done());
    CHECK("EP147 comes back however the program ends (ExecStopPost)",
          recorded("--property=ExecStopPost=/bin/systemctl start EP147.service"));
    CHECK("the program runs after EP147 stops", recorded("systemctl stop EP147.service; exec \"$0\" \"$@\"") &&
                                                    recorded(p));

    /* with the handoff guard on the deck: it rides in the service, and its restore is ExecStopPost */
    put_file(g_guard, "#!/bin/sh\nexit 0\n", 0644); /* as a FAT stick may leave it: not executable */
    unlink(g_sdrun_args);
    char want_post[PATH_MAX + 64], want_sh[PATH_MAX + 128];
    snprintf(want_post, sizeof want_post, "--property=ExecStopPost=%s --restore", g_guard_run);
    snprintf(want_sh, sizeof want_sh, "systemctl stop EP147.service; %s azmod-handoff & exec \"$0\" \"$@\"", g_guard_run);
    CHECK("with the guard: started", sdk_handoff("mod-a", p, NULL) == 0 && handoff_done());
    struct stat gst;
    CHECK("with the guard: it runs from its own executable copy",
          stat(g_guard_run, &gst) == 0 && (gst.st_mode & 0777) == 0755 && access(g_guard_run, X_OK) == 0);
    CHECK("with the guard: its restore brings EP147 back (ExecStopPost)", recorded(want_post));
    CHECK("with the guard: it watches for BACK beside the program", recorded(want_sh) && recorded(p));
    unlink(g_guard);
    unlink(g_guard_run);

    /* 5. reload never interrupts a set */
    g_any_loaded = 1;
    g_sdk_reload_req = 0;
    sdk_reload("mod-a");
    CHECK("reload with a track loaded: refused", g_sdk_reload_req == 0);
    g_any_loaded = 0;
    sdk_reload("mod-a");
    CHECK("reload with no track loaded: requested", g_sdk_reload_req == 1);

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    if (system(cmd)) {
    }
    return ut_summary();
}
