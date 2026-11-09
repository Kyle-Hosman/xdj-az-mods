/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file sdk_proc.c
 * @brief Community mod services: other programs (launch, launch_alive, launch_stop, handoff)
 *        and reload
 *
 * A mod never starts a program from inside EP147 by itself. launch() checks that the program
 * is the mod's own file (inside its assets_dir or data_dir, after resolving links), then starts
 * it with posix_spawn (no fork of EP147) through `systemd-run --scope`, so it runs in its own
 * cgroup outside EP147's, without the mods' LD_PRELOAD, at normal (not real-time) scheduling,
 * with the mod's nice / cpu / memory options. A waiter thread per program reaps it, and only
 * the mod that launched a pid may ask about it or stop it.
 *
 * handoff() runs a program in EP147's place: a transient systemd service stops EP147, runs the
 * program, and starts EP147 again when it exits; ExecStopPost starts EP147 even if the program
 * or the service is killed. It needs the DJ's yes and is refused while a deck plays. The
 * handoff guard (azhandoff) rides in the same service: BACK held for 3 seconds stops a
 * program that does not end, and its restore starts EP147, checks that it stays up, retries
 * once and reboots the deck as the last resort.
 *
 * reload() is the Mod Menu's Reload, refused while a track is loaded (the restart policy).
 * Included by community.c ahead of the services table; uses its cm_* helpers.
 */
#include <dirent.h>
#include <sched.h>

#define SDK_PROCS 32 /* programs tracked at once, all mods */

static struct sdk_proc {
    char id[33];          /* the mod that launched it; "" = free slot */
    pid_t pid;
    _Atomic int running;  /* 1 until the waiter saw it exit */
} g_sdk_proc[SDK_PROCS];
static pthread_mutex_t g_sdk_proc_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned g_sdk_proc_seq;

/* 1 = `pid` was launched by mod `id` and is still running (surfaces: xwin_show's window owner). */
static int sdk_proc_owns(const char *id, int pid) {
    if (!id || pid <= 0)
        return 0;
    int yes = 0;
    pthread_mutex_lock(&g_sdk_proc_mu);
    for (int i = 0; i < SDK_PROCS; i++)
        if (g_sdk_proc[i].pid == pid && !strcmp(g_sdk_proc[i].id, id) &&
            atomic_load(&g_sdk_proc[i].running))
            yes = 1;
    pthread_mutex_unlock(&g_sdk_proc_mu);
    return yes;
}

/* `path` resolved, and inside `dir` (also resolved). out gets the resolved path. */
static int sdk_path_inside(const char *path, const char *dir, char *out, size_t n) {
    char rp[PATH_MAX], rd[PATH_MAX];
    if (!path || !dir || path[0] != '/' || !realpath(path, rp) || !realpath(dir, rd))
        return 0;
    size_t k = strlen(rd);
    if (strncmp(rp, rd, k) != 0 || rp[k] != '/')
        return 0;
    if (snprintf(out, n, "%s", rp) >= (int) n)
        return 0;
    return 1;
}

/* The mod's own program: a regular, executable file inside its assets_dir or data_dir. */
static int sdk_own_program(const char *id, const char *path, char *out, size_t n) {
    pthread_mutex_lock(&g_cm_mu);
    const char *a = cm_assets_dir_u(id), *d = cm_data_dir_u(id);
    int ok = (a && sdk_path_inside(path, a, out, n)) || (d && sdk_path_inside(path, d, out, n));
    pthread_mutex_unlock(&g_cm_mu);
    struct stat st;
    return ok && stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0;
}

/* A log file the program may write: inside the mod's data_dir (the file may not exist yet, so
 * its folder is what is resolved). The name itself must not lead out: not "." or "..", and an
 * existing file must be a plain one with no other name (no link out, no hard link). */
static int sdk_own_log(const char *id, const char *path, char *out, size_t n) {
    char dir[PATH_MAX];
    const char *slash = path ? strrchr(path, '/') : NULL;
    if (!slash || slash == path || !slash[1] || (size_t) (slash - path) >= sizeof dir ||
        !strcmp(slash + 1, ".") || !strcmp(slash + 1, ".."))
        return 0;
    memcpy(dir, path, (size_t) (slash - path));
    dir[slash - path] = 0;
    pthread_mutex_lock(&g_cm_mu);
    const char *d = cm_data_dir_u(id);
    char rd[PATH_MAX], rdir[PATH_MAX];
    int ok = d && realpath(d, rd) && realpath(dir, rdir) &&
             (!strcmp(rdir, rd) || (!strncmp(rdir, rd, strlen(rd)) && rdir[strlen(rd)] == '/'));
    pthread_mutex_unlock(&g_cm_mu);
    if (!ok || snprintf(out, n, "%s/%s", rdir, slash + 1) >= (int) n)
        return 0;
    struct stat st;
    return lstat(out, &st) != 0 ? errno == ENOENT : S_ISREG(st.st_mode) && st.st_nlink == 1;
}

/* posix_spawn argv with: no LD_PRELOAD in the environment, normal scheduling (EP147's threads
 * may be real-time and that must not carry over), default signal handling and an empty signal
 * mask, stdout/stderr to `log` (appended) or /dev/null. Returns the pid or -1. */
/* ---- the program's log: capped, rotated once ---------------------------------------------
 * The program's stdout and stderr go into a pipe; this thread copies them to the log and, when
 * the log would pass SDK_LOG_CAP, moves it to <log>.1 (replacing the previous one) and starts
 * a new one. A program that writes faster than the stick takes it waits on its pipe. */
#ifndef SDK_LOG_CAP
#define SDK_LOG_CAP (1024 * 1024)
#endif
struct sdk_logcopy {
    int fd; /* the pipe's read end */
    char path[PATH_MAX];
};

static int sdk_log_open(const char *path, off_t *size) {
    /* never through a link: the name was checked at launch, the file can change since */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644); /* logcap-ok: sdk_log_copier rotates at SDK_LOG_CAP */
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1)) {
        close(fd);
        fd = -1;
    }
    *size = fd >= 0 ? st.st_size : 0;
    return fd;
}

static void *sdk_log_copier(void *arg) {
    struct sdk_logcopy *lc = arg;
    char old[PATH_MAX + 4], buf[4096];
    snprintf(old, sizeof old, "%s.1", lc->path);
    off_t size;
    int out = sdk_log_open(lc->path, &size);
    for (;;) {
        ssize_t n = read(lc->fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break; /* the program (and everything it started) closed its output */
        if (out >= 0 && size + n > SDK_LOG_CAP && size > 0) {
            close(out);
            rename(lc->path, old);
            out = sdk_log_open(lc->path, &size);
        }
        if (out >= 0 && write(out, buf, (size_t) n) == n)
            size += n;
    }
    if (out >= 0)
        close(out);
    close(lc->fd);
    free(lc);
    return NULL;
}

static int sdk_detach(void *(*fn)(void *), void *arg);

/* EP147 opens most of its descriptors without close-on-exec: the control-bus devices, sockets,
 * files on the sticks. A program must not inherit them (one holding a stick's file open keeps
 * the stick from being ejected cleanly), so every inherited descriptor above stderr that would
 * survive the exec is closed in the child. One opened by another thread after this list is made
 * can still slip through; one closed meanwhile is skipped by the child (glibc ignores EBADF). */
static void sdk_close_inherited(posix_spawn_file_actions_t *fa) {
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return;
    int self = dirfd(d), n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 4096) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        int fd = atoi(e->d_name), fl;
        if (fd <= 2 || fd == self || (fl = fcntl(fd, F_GETFD)) < 0 || (fl & FD_CLOEXEC))
            continue;
        if (posix_spawn_file_actions_addclose(fa, fd) == 0)
            n++;
    }
    closedir(d);
}

static pid_t sdk_spawn_clean(char *const argv[], const char *log) {
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    int pfd[2] = {-1, -1};
    if (log) { /* the copier first: with none, the program would die on its first write */
        struct sdk_logcopy *lc = malloc(sizeof *lc);
        if (!lc || pipe2(pfd, O_CLOEXEC) != 0) {
            free(lc);
            return -1;
        }
        lc->fd = pfd[0];
        snprintf(lc->path, sizeof lc->path, "%s", log);
        if (sdk_detach(sdk_log_copier, lc) != 0) {
            close(pfd[0]), close(pfd[1]), free(lc);
            return -1;
        }
    }
    if (posix_spawn_file_actions_init(&fa) != 0) {
        if (log)
            close(pfd[1]); /* the copier sees end of file and ends */
        return -1;
    }
    if (posix_spawnattr_init(&at) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        if (log)
            close(pfd[1]);
        return -1;
    }
    int ok = (log ? posix_spawn_file_actions_adddup2(&fa, pfd[1], 1) == 0
                  : posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0) == 0) &&
             posix_spawn_file_actions_adddup2(&fa, 1, 2) == 0 &&
             posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0) == 0;
    if (ok)
        sdk_close_inherited(&fa); /* after the dup2s: stdout and stderr are set up first */
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    struct sched_param sp = {0};
    ok = ok && posix_spawnattr_setsigmask(&at, &none) == 0 &&
         posix_spawnattr_setsigdefault(&at, &all) == 0 &&
         posix_spawnattr_setschedpolicy(&at, SCHED_OTHER) == 0 &&
         posix_spawnattr_setschedparam(&at, &sp) == 0 &&
         posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                           POSIX_SPAWN_SETSCHEDULER | POSIX_SPAWN_SETSCHEDPARAM) == 0;
    size_t ne = 0;
    while (environ && environ[ne])
        ne++;
    char **envp = ok ? malloc((ne + 1) * sizeof *envp) : NULL;
    pid_t pid = -1;
    if (envp) {
        size_t k = 0;
        for (size_t i = 0; i < ne; i++)
            if (strncmp(environ[i], "LD_PRELOAD=", 11) != 0)
                envp[k++] = environ[i];
        envp[k] = NULL;
        if (posix_spawn(&pid, argv[0], &fa, &at, argv, envp) != 0)
            pid = -1;
        free(envp);
    }
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    if (log)
        close(pfd[1]); /* the child holds the only write end now; no child: the copier ends */
    return pid;
}

static void *sdk_proc_waiter(void *arg) {
    struct sdk_proc *p = arg;
    pid_t pid = p->pid; /* set before this thread was created; the slot is not reused until running is 0 */
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    atomic_store(&p->running, 0); /* exited (or already reaped: SIGCHLD ignored) */
    return NULL;
}

static int sdk_detach(void *(*fn)(void *), void *arg) {
    pthread_t th;
    pthread_attr_t at;
    if (pthread_attr_init(&at) != 0)
        return -1;
    int ok = pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED) == 0 &&
             az_thread_create(&th, &at, fn, arg) == 0;
    pthread_attr_destroy(&at);
    return ok ? 0 : -1;
}

#define SDK_ARGV_MAX 32 /* the mod's own arguments */
#ifndef SDK_SYSTEMD_RUN
#define SDK_SYSTEMD_RUN "/usr/bin/systemd-run"
#endif

static int sdk_launch(const char *id, const char *path, const char *const argv[],
                      const az_launch_opts *opts) {
    cm_mod *c = cm_self(id, CM_RA(), "launch");
    if (!c)
        return -1;
    char prog[PATH_MAX], logp[PATH_MAX];
    if (!sdk_own_program(id, path, prog, sizeof prog)) {
        cm_logf(c, 1, "launch refused: %s is not an executable file in this mod's assets or data folder",
                path ? path : "(null)");
        return -1;
    }
    int nice_v = 0;
    unsigned mask = 0, mem = 0;
    const char *log = NULL;
    if (opts && opts->size >= (int) (offsetof(az_launch_opts, log_path) + sizeof opts->log_path)) {
        nice_v = opts->nice < 0 ? 0 : opts->nice > 19 ? 19
                                                      : opts->nice;
        mask = opts->cpu_mask;
        mem = opts->mem_mb;
        if (opts->log_path) {
            if (!sdk_own_log(id, opts->log_path, logp, sizeof logp)) {
                cm_logf(c, 1, "launch refused: the log %s is not inside this mod's data folder",
                        opts->log_path);
                return -1;
            }
            log = logp;
        }
    }
    char unit[96], memprop[40], nicebuf[8], maskbuf[16];
    snprintf(unit, sizeof unit, "--unit=azmod-%s-%u", id, atomic_fetch_add(&g_sdk_proc_seq, 1));
    const char *av[SDK_ARGV_MAX + 16];
    int n = 0;
    av[n++] = SDK_SYSTEMD_RUN;
    av[n++] = "--scope";
    av[n++] = "--quiet";
    av[n++] = unit;
    if (mem) {
        snprintf(memprop, sizeof memprop, "--property=MemoryLimit=%uM", mem);
        av[n++] = memprop;
    }
    av[n++] = "--";
    if (nice_v) {
        snprintf(nicebuf, sizeof nicebuf, "%d", nice_v);
        av[n++] = "/usr/bin/nice";
        av[n++] = "-n";
        av[n++] = nicebuf;
    }
    if (mask) {
        snprintf(maskbuf, sizeof maskbuf, "%x", mask);
        av[n++] = "/usr/bin/taskset";
        av[n++] = maskbuf;
    }
    av[n++] = prog;
    for (int i = 0; argv && argv[i]; i++) {
        if (i >= SDK_ARGV_MAX) {
            cm_logf(c, 1, "launch refused: more than %d arguments", SDK_ARGV_MAX);
            return -1;
        }
        av[n++] = argv[i];
    }
    av[n] = NULL;
    pthread_mutex_lock(&g_sdk_proc_mu);
    struct sdk_proc *p = NULL;
    for (int i = 0; i < SDK_PROCS && !p; i++)
        if (!atomic_load(&g_sdk_proc[i].running))
            p = &g_sdk_proc[i];
    pid_t pid = -1;
    if (p) {
        pid = sdk_spawn_clean((char *const *) av, log);
        if (pid > 0) {
            snprintf(p->id, sizeof p->id, "%s", id);
            p->pid = pid;
            atomic_store(&p->running, 1);
            if (sdk_detach(sdk_proc_waiter, p) != 0)
                cm_logf(c, 1, "launch: no waiter thread for pid %d; it is never reaped", (int) pid);
        }
    }
    pthread_mutex_unlock(&g_sdk_proc_mu);
    if (!p)
        cm_logf(c, 1, "launch refused: %d programs are already running", SDK_PROCS);
    else if (pid < 0)
        cm_logf(c, 1, "launch of %s failed: %s", prog, strerror(errno));
    else
        cm_logf(c, 1, "launch %s: pid %d", prog, (int) pid);
    return pid > 0 ? (int) pid : -1;
}

static struct sdk_proc *sdk_find_own(const char *id, int pid) {
    for (int i = 0; i < SDK_PROCS; i++)
        if (g_sdk_proc[i].pid == pid && pid > 0 && !strcmp(g_sdk_proc[i].id, id))
            return &g_sdk_proc[i];
    return NULL;
}

static int sdk_launch_alive(const char *id, int pid) {
    if (!cm_self(id, CM_RA(), "launch_alive"))
        return 0;
    pthread_mutex_lock(&g_sdk_proc_mu);
    struct sdk_proc *p = sdk_find_own(id, pid);
    int alive = p && atomic_load(&p->running);
    pthread_mutex_unlock(&g_sdk_proc_mu);
    return alive;
}

struct sdk_stop {
    struct sdk_proc *p;
    pid_t pid;
};
/* the slot still holds that program: running, and not reused by a new launch (pid is written
 * under the lock) */
static int sdk_still_running(const struct sdk_stop *s) {
    pthread_mutex_lock(&g_sdk_proc_mu);
    int same = atomic_load(&s->p->running) && s->p->pid == s->pid;
    pthread_mutex_unlock(&g_sdk_proc_mu);
    return same;
}
/* SIGKILL after the 2 s grace period, unless it exited: and only if the slot still holds the
 * same program (a slot freed by the exit may be reused by a new launch meanwhile). */
static void *sdk_stop_later(void *arg) {
    struct sdk_stop *s = arg;
    for (int i = 0; i < 20 && sdk_still_running(s); i++)
        usleep(100000);
    pthread_mutex_lock(&g_sdk_proc_mu);
    if (atomic_load(&s->p->running) && s->p->pid == s->pid)
        kill(s->pid, SIGKILL);
    pthread_mutex_unlock(&g_sdk_proc_mu);
    free(s);
    return NULL;
}

/* SIGTERM a running program, SIGKILL after the grace period. Under g_sdk_proc_mu. 0 = sent. */
static int sdk_proc_term_locked(struct sdk_proc *p) {
    pid_t pid = p->pid;
    if (!atomic_load(&p->running) || kill(pid, SIGTERM) != 0)
        return -1;
    struct sdk_stop *later = malloc(sizeof *later);
    if (later)
        *later = (struct sdk_stop) {p, pid};
    if (!later || sdk_detach(sdk_stop_later, later) != 0) {
        free(later);
        kill(pid, SIGKILL); /* no thread for the grace period: stop it now */
    }
    return 0;
}
/* A mod turned off live: every program it launched that still runs is stopped. Returns how many. */
static int sdk_proc_forget(const char *id) {
    int n = 0;
    pthread_mutex_lock(&g_sdk_proc_mu);
    for (int i = 0; i < SDK_PROCS; i++)
        if (g_sdk_proc[i].id[0] && !strcmp(g_sdk_proc[i].id, id) && sdk_proc_term_locked(&g_sdk_proc[i]) == 0)
            n++;
    pthread_mutex_unlock(&g_sdk_proc_mu);
    return n;
}

static int sdk_launch_stop(const char *id, int pid) {
    cm_mod *c = cm_self(id, CM_RA(), "launch_stop");
    if (!c)
        return -1;
    pthread_mutex_lock(&g_sdk_proc_mu);
    struct sdk_proc *p = sdk_find_own(id, pid);
    int rc = p ? sdk_proc_term_locked(p) : -1;
    pthread_mutex_unlock(&g_sdk_proc_mu);
    if (rc)
        cm_logf(c, 1, "launch_stop %d refused: not a running program this mod launched", pid);
    return rc;
}

/* ---- handoff ------------------------------------------------------------------------- */
/* The handoff guard ships in MODS/deck/lib, which the loader keeps in the stage. */
#ifndef SDK_GUARD_STAGE
#define SDK_GUARD_STAGE "/home/root/settings/az-usbload/deck/lib/az-handoff-guard"
#endif
/* The guard runs from a fresh copy in /run (tmpfs), mode 0755: the stage keeps whatever mode the
 * stick's file system gave it, as the stick's dropbear does (the loader runs that from /run too). */
#ifndef SDK_GUARD_RUN
#define SDK_GUARD_RUN "/run/az-handoff-guard"
#endif
static const char *sdk_guard_source(void) {
#ifdef SDK_GUARD_DEV
    if (access(SDK_GUARD_DEV, R_OK) == 0)
        return SDK_GUARD_DEV;
#endif
    return access(SDK_GUARD_STAGE, R_OK) == 0 ? SDK_GUARD_STAGE : NULL;
}
/* Copy the guard to SDK_GUARD_RUN; its path, or NULL (no guard, or the copy failed). Handoff
 * thread only (file I/O). */
static const char *sdk_guard_path(void) {
    const char *src = sdk_guard_source();
    if (!src)
        return NULL;
    char tmp[PATH_MAX];
    if ((size_t) snprintf(tmp, sizeof tmp, "%s.new", SDK_GUARD_RUN) >= sizeof tmp)
        return NULL;
    int in = open(src, O_RDONLY | O_CLOEXEC), out = -1, ok = 0;
    if (in >= 0)
        out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (out >= 0) {
        char b[16384];
        ssize_t n;
        ok = 1;
        while ((n = read(in, b, sizeof b)) > 0)
            if (write(out, b, (size_t) n) != n) {
                ok = 0;
                break;
            }
        ok = ok && n == 0 && fchmod(out, 0755) == 0;
        ok = close(out) == 0 && ok;
    }
    if (in >= 0)
        close(in);
    if (!ok || rename(tmp, SDK_GUARD_RUN) != 0) {
        unlink(tmp);
        return NULL;
    }
    return SDK_GUARD_RUN;
}
/* The dialog handoff asks through: g_sdk_confirm_core (azmod_internal.h). */
struct sdk_handoff {
    char id[33], prog[PATH_MAX];
    char args[SDK_ARGV_MAX][128];
    int nargs;
};
static _Atomic int g_sdk_handoff_busy;

/* Any deck playing? The playhead of every deck, read twice 250 ms apart: a moving playhead is
 * playing. Unknown (no azstem) counts as playing. Not on a UI or audio thread. */
static int sdk_any_deck_playing(void) {
    static long long (*obs)(int);
    if (!obs)
        *(void **) &obs = dlsym(RTLD_DEFAULT, "azstem_play_obs");
    if (!obs)
        return 1;
    long long a[AZMOD_STEM_DECKS];
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        a[d] = obs(d);
    usleep(250000);
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        if (a[d] >= 0 && obs(d) != a[d])
            return 1;
    return 0;
}

static void *sdk_handoff_run(void *arg) {
    struct sdk_handoff *h = arg;
    cm_mod *c = cm_find(h->id);
    if (sdk_any_deck_playing()) {
        cm_logf(c, 1, "handoff refused: a deck is playing");
        goto out;
    }
    /* The program runs in a transient service, outside EP147 and its environment (so without
     * the mods' preload). The shell stops EP147 first, starts the guard beside the program, and
     * passes the program and its arguments through as "$0" "$@", so nothing is quoted.
     * ExecStopPost brings EP147 back however the service ends: the program exits, crashes, or
     * is stopped by the guard. Without the guard on the deck there is no BACK escape, and the
     * restore is a plain start. */
    const char *guard = sdk_guard_path();
    char post[PATH_MAX + 48], sh[PATH_MAX + 96];
    if (guard) {
        snprintf(post, sizeof post, "--property=ExecStopPost=%s --restore", guard);
        snprintf(sh, sizeof sh, "systemctl stop EP147.service; %s azmod-handoff & exec \"$0\" \"$@\"",
                 guard);
    } else {
        cm_logf(c, 1, "handoff: no handoff guard on this deck (MODS/deck/lib/az-handoff-guard): "
                      "holding BACK will not end the program");
        snprintf(post, sizeof post, "--property=ExecStopPost=/bin/systemctl start EP147.service");
        snprintf(sh, sizeof sh, "systemctl stop EP147.service; exec \"$0\" \"$@\"");
    }
    const char *av[SDK_ARGV_MAX + 16];
    int n = 0;
    av[n++] = SDK_SYSTEMD_RUN;
    av[n++] = "--unit=azmod-handoff";
    av[n++] = "--collect";
    av[n++] = "--quiet";
    av[n++] = post;
    av[n++] = "--setenv=DISPLAY=:0";
    av[n++] = "--setenv=LANG=C.UTF-8";
    av[n++] = "--";
    av[n++] = "/bin/sh";
    av[n++] = "-c";
    av[n++] = sh;
    av[n++] = h->prog;
    for (int i = 0; i < h->nargs; i++)
        av[n++] = h->args[i];
    av[n] = NULL;
    pid_t pid = sdk_spawn_clean((char *const *) av, NULL);
    int st = 0;
    if (pid > 0)
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
    if (pid < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
        cm_logf(c, 1, "handoff: systemd-run did not start %s (status %d)", h->prog, st);
    else
        cm_logf(c, 1, "handoff: %s started; EP147 comes back when it exits", h->prog);
out:
    free(h);
    atomic_store(&g_sdk_handoff_busy, 0);
    return NULL;
}

static void sdk_handoff_answer(int yes, void *user) {
    struct sdk_handoff *h = user;
    if (!yes || sdk_detach(sdk_handoff_run, h) != 0) {
        free(h);
        atomic_store(&g_sdk_handoff_busy, 0);
    }
}

static int sdk_handoff(const char *id, const char *path, const char *const argv[]) {
    cm_mod *c = cm_self(id, CM_RA(), "handoff");
    if (!c)
        return -1;
    __atomic_store_n(&c->sticky, 1, __ATOMIC_RELAXED); /* lasts until the next start: no live stop */
    struct sdk_handoff *h = calloc(1, sizeof *h);
    if (!h)
        return -1;
    snprintf(h->id, sizeof h->id, "%s", id);
    if (!sdk_own_program(id, path, h->prog, sizeof h->prog)) {
        cm_logf(c, 1, "handoff refused: %s is not an executable file in this mod's assets or data folder",
                path ? path : "(null)");
        free(h);
        return -1;
    }
    for (int i = 0; argv && argv[i]; i++) {
        if (i >= SDK_ARGV_MAX || strlen(argv[i]) >= sizeof h->args[0]) {
            cm_logf(c, 1, "handoff refused: too many or too long arguments");
            free(h);
            return -1;
        }
        snprintf(h->args[i], sizeof h->args[i], "%s", argv[i]);
        h->nargs = i + 1;
    }
    int (*confirm)(const char *, const char *, const char *, void (*)(int, void *), void *) =
        __atomic_load_n(&g_sdk_confirm_core, __ATOMIC_ACQUIRE);
    if (!confirm) {
        cm_logf(c, 1, "handoff refused: this core has no confirm dialog to ask the user with");
        free(h);
        return -1;
    }
    if (atomic_exchange(&g_sdk_handoff_busy, 1)) {
        cm_logf(c, 1, "handoff refused: another handoff is in progress");
        free(h);
        return -1;
    }
    if (confirm(c->name[0] ? c->name : id,
                "The deck program stops while this runs, and comes back when it exits. "
                "Hold BACK for 3 seconds to return to the deck.",
                "Start",
                sdk_handoff_answer, h) != 0) {
        cm_logf(c, 1, "handoff refused: the confirm dialog is busy with another question");
        free(h);
        atomic_store(&g_sdk_handoff_busy, 0);
        return -1;
    }
    return 0;
}

/* ---- reload --------------------------------------------------------------------------- */
/* Read by the overlay thread (mod_menu_overlay.c, ov_poll_inputs), which runs the Reload. */
static void sdk_reload(const char *id) {
    cm_mod *c = cm_self(id, CM_RA(), "reload");
    if (!c)
        return;
    if (azcm_any_loaded()) {
        cm_logf(c, 1, "reload refused: a track is loaded (a Reload never interrupts a set)");
        return;
    }
    cm_logf(c, 1, "reload requested");
    __atomic_store_n(&g_sdk_reload_req, 1, __ATOMIC_RELEASE);
}
