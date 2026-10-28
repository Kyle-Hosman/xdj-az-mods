#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# tsan-skip-lint.sh <program>... : the test programs of the suites the ThreadSanitizer pass does not
# run (run.sh tsan_skip) can produce no ThreadSanitizer report. ThreadSanitizer reports data races
# between threads, misuse and lock-order inversions of locks, and signal-unsafe calls in signal
# handlers; a program that imports no call able to start a second thread or process, take a lock or
# install a signal handler can hit none of them. Each program's dynamic imports (nm -u) are checked
# against that list. A program that is missing, is not a dynamically linked executable (nothing
# imported, so nothing to check) or imports a listed call fails the check.
# Exit 0 = every program checked and clean, 1 = a listed import, 2 = a program could not be checked.
set -uo pipefail
[ $# -gt 0 ] || { echo "FAIL  tsan-skip-lint: no programs named"; exit 2; }
command -v nm >/dev/null 2>&1 || { echo "FAIL  tsan-skip-lint: nm not found"; exit 2; }
# threads (POSIX, C11, clone), processes (fork, spawn, exec, system, popen), locks and condition
# variables (POSIX, C11, semaphores), signal handlers, timers that call back on a thread, POSIX
# asynchronous I/O and message queue notifications (both may run a callback on a new thread), and
# dlopen (a loaded library can start threads of its own)
DENY='^(pthread_|thrd_|mtx_|cnd_|call_once$|sem_|clone$|__clone|fork$|vfork$|__fork|posix_spawn|system$|popen$|exec[lv]|fexecve$|signal$|sigaction$|__sysv_signal$|bsd_signal$|sigset$|timer_create$|aio_|lio_listio$|mq_notify$|dlopen$|dlmopen$)'
# Mach-O symbol names carry one leading underscore over the C name
STRIP='s/^//'
[ "$(uname -s)" != Darwin ] || STRIP='s/^_//'
rc=0
for p in "$@"; do
    n=${p##*/}
    if [ ! -f "$p" ] || [ ! -x "$p" ]; then
        echo "FAIL  tsan-skip-lint: $n is not built ($p)"
        rc=2
        continue
    fi
    imports=$(nm -u "$p" 2>/dev/null | awk '{print $NF}' | sed -e 's/@.*//' -e "$STRIP" | sort -u) || imports=
    if [ -z "$imports" ]; then
        echo "FAIL  tsan-skip-lint: $n imports nothing (static or not an executable): cannot be checked"
        rc=2
        continue
    fi
    bad=$(grep -E "$DENY" <<<"$imports" | tr '\n' ' ')
    if [ -n "$bad" ]; then
        echo "FAIL  tsan-skip-lint: $n imports $bad(it can start a thread or process, lock or take a signal: run it under ThreadSanitizer)"
        [ "$rc" = 2 ] || rc=1
    else
        echo "PASS  tsan-skip-lint: $n starts no thread or process, takes no lock, installs no signal handler ($(wc -l <<<"$imports" | tr -d ' ') imported symbols checked)"
    fi
done
exit "$rc"
