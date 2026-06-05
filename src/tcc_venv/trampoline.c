/*
 * tcc-venv trampoline — a stable, codesigned launcher for a uv/venv Python app
 * so macOS TCC (Full Disk Access / Automation) attributes grants to THIS binary
 * instead of the moving Homebrew-uv / version-pinned-python paths.
 *
 * Installed by `tcc-venv` as <venv>/bin/python-tcc-<project> (macOS). It:
 *   - self-locates its venv from its own executable path (NOT $VIRTUAL_ENV, which
 *     could leak from another project and run the wrong interpreter under this
 *     binary's identity — it is only honored if it AGREES with the self-located venv),
 *   - posix_spawns <venv>/bin/python with argv[1:] appended (stays the live parent
 *     so the child inherits this binary's TCC grant — must NOT exec into python),
 *   - runs the child in its own process group and (when interactive) hands it the
 *     controlling terminal, so terminal signals reach the child once — not the
 *     wrapper and then the child again,
 *   - forwards directed signals to the child's process group and propagates exit.
 *
 * The bytes are project-INDEPENDENT (the per-project identity comes from codesign
 * --identifier + the on-disk filename, applied by the installer), so one cached
 * build is copied to every venv.
 *
 * On non-macOS this file is unused — the installer symlinks python-tcc -> python
 * directly — but the code still compiles to a plain exec of the venv python.
 */
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

static const int FORWARDED_SIGNALS[] = {
    SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2,
};
#define N_FORWARDED ((int)(sizeof(FORWARDED_SIGNALS) / sizeof(FORWARDED_SIGNALS[0])))

static volatile sig_atomic_t child_pid = -1;

static void forward_signal(int signo) {
    pid_t pid = (pid_t)child_pid;
    if (pid > 0) {
        /* Child leads its own process group; signal the whole group so any
         * grandchildren are torn down too. */
        kill(-pid, signo);
    }
}

static void install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = forward_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (int i = 0; i < N_FORWARDED; i++) {
        sigaction(FORWARDED_SIGNALS[i], &action, NULL);
    }
}

/* malloc'd absolute path to this executable, symlinks resolved, or NULL. */
static char *self_path(void) {
    char *raw = NULL;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size); /* sets size to the required length */
    raw = malloc(size ? size : 1);
    if (raw == NULL) {
        return NULL;
    }
    if (_NSGetExecutablePath(raw, &size) != 0) {
        free(raw);
        return NULL;
    }
#else
    size_t cap = 1024;
    for (;;) {
        char *buf = malloc(cap);
        if (buf == NULL) {
            return NULL;
        }
        ssize_t n = readlink("/proc/self/exe", buf, cap);
        if (n < 0) {
            free(buf);
            return NULL;
        }
        if ((size_t)n < cap) {
            buf[n] = '\0';
            raw = buf;
            break;
        }
        free(buf);
        cap *= 2;
    }
#endif
    char *resolved = realpath(raw, NULL);
    if (resolved != NULL) {
        free(raw);
        return resolved;
    }
    return raw; /* fall back to the unresolved path if realpath fails */
}

/* Strip the last path component in place (dirname). */
static int strip_component(char *path) {
    char *slash = strrchr(path, '/');
    if (slash == NULL || slash == path) {
        return -1;
    }
    *slash = '\0';
    return 0;
}

/* malloc'd "dir/leaf", or NULL. */
static char *path_join(const char *dir, const char *leaf) {
    size_t n = strlen(dir) + 1 + strlen(leaf) + 1;
    char *out = malloc(n);
    if (out == NULL) {
        return NULL;
    }
    snprintf(out, n, "%s/%s", dir, leaf);
    return out;
}

int main(int argc, char *argv[]) {
    /* self = <venv>/bin/python-tcc-<project>  ->  venv = dirname(dirname(self)) */
    char *venv = self_path();
    if (venv == NULL) {
        fprintf(stderr, "python-tcc: cannot resolve own executable path\n");
        return 127;
    }
    if (strip_component(venv) != 0 || strip_component(venv) != 0) {
        fprintf(stderr, "python-tcc: unexpected install location (not <venv>/bin/...)\n");
        free(venv);
        return 127;
    }

    /* Security: never let a stray $VIRTUAL_ENV redirect us to another venv. */
    const char *venv_env = getenv("VIRTUAL_ENV");
    if (venv_env != NULL && venv_env[0] != '\0') {
        char *env_resolved = realpath(venv_env, NULL);
        if (env_resolved != NULL) {
            int mismatch = strcmp(env_resolved, venv) != 0;
            if (mismatch) {
                fprintf(
                    stderr,
                    "python-tcc: refusing to run — $VIRTUAL_ENV (%s) disagrees with my venv (%s)\n",
                    env_resolved, venv);
            }
            free(env_resolved);
            if (mismatch) {
                free(venv);
                return 125;
            }
        }
    }

    char *python = path_join(venv, "bin/python");
    if (python == NULL) {
        fprintf(stderr, "python-tcc: out of memory\n");
        free(venv);
        return 127;
    }
    if (access(python, X_OK) != 0) {
        /* Fall back to python3 if a bare `python` is absent. */
        char *python3 = path_join(venv, "bin/python3");
        if (python3 != NULL && access(python3, X_OK) == 0) {
            free(python);
            python = python3;
        } else {
            fprintf(stderr, "python-tcc: no venv python at %s\n", python);
            free(python3);
            free(python);
            free(venv);
            return 127;
        }
    }
    free(venv);

    /* child argv: python + argv[1:]  (argc may be 0 in a pathological exec). */
    int passthrough = argc > 1 ? argc - 1 : 0;
    char **child_argv = calloc((size_t)passthrough + 2, sizeof(char *));
    if (child_argv == NULL) {
        fprintf(stderr, "python-tcc: out of memory\n");
        free(python);
        return 127;
    }
    child_argv[0] = python;
    for (int i = 0; i < passthrough; i++) {
        child_argv[i + 1] = argv[i + 1];
    }
    child_argv[passthrough + 1] = NULL;

#ifndef __APPLE__
    /* No TCC off-mac: just become python (cheaper, no supervisor needed). */
    execv(python, child_argv);
    fprintf(stderr, "python-tcc: execv %s failed: %s\n", python, strerror(errno));
    free(child_argv);
    free(python);
    return 127;
#else
    /* Don't let the wrapper be stopped if it writes to the tty while the child
     * owns the terminal foreground (set below). */
    signal(SIGTTOU, SIG_IGN);

    /* Block the forwarded signals across spawn + child_pid assignment so a
     * signal arriving before the handlers are live can't kill only the wrapper
     * and orphan python (the handlers are a no-op until child_pid is valid). */
    sigset_t block, prev;
    sigemptyset(&block);
    for (int i = 0; i < N_FORWARDED; i++) {
        sigaddset(&block, FORWARDED_SIGNALS[i]);
    }
    sigprocmask(SIG_BLOCK, &block, &prev);

    /* Run the child in its own process group so terminal-generated signals are
     * delivered to it once, not to the wrapper and then forwarded again. Reset
     * its signal mask to the pre-block set (SETSIGMASK) and its dispositions to
     * default (SETSIGDEF) — otherwise the child inherits our temporarily-blocked
     * mask and never sees a forwarded SIGTERM. */
    sigset_t child_defaults;
    sigemptyset(&child_defaults);
    for (int i = 0; i < N_FORWARDED; i++) {
        sigaddset(&child_defaults, FORWARDED_SIGNALS[i]);
    }
    sigaddset(&child_defaults, SIGTTOU);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(
        &attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    posix_spawnattr_setpgroup(&attr, 0); /* child becomes its own group leader */
    posix_spawnattr_setsigmask(&attr, &prev);
    posix_spawnattr_setsigdefault(&attr, &child_defaults);

    pid_t pid;
    int spawn_status = posix_spawn(&pid, python, NULL, &attr, child_argv, environ);
    posix_spawnattr_destroy(&attr);
    free(child_argv);
    if (spawn_status != 0) {
        sigprocmask(SIG_SETMASK, &prev, NULL);
        fprintf(stderr, "python-tcc: failed to spawn %s: %s\n", python, strerror(spawn_status));
        free(python);
        return 127;
    }

    child_pid = (sig_atomic_t)pid;

    /* Hand the controlling terminal's foreground to the child's group so an
     * interactive REPL / input() keeps working and Ctrl-C reaches the child. */
    int interactive = isatty(STDIN_FILENO);
    if (interactive) {
        tcsetpgrp(STDIN_FILENO, pid); /* pid == the child's pgid */
    }

    install_signal_handlers();
    sigprocmask(SIG_SETMASK, &prev, NULL);

    int status;
    for (;;) {
        pid_t waited = waitpid(pid, &status, 0);
        if (waited == pid) {
            break;
        }
        if (waited == -1 && errno == EINTR) {
            continue;
        }
        if (waited == -1) {
            fprintf(stderr, "python-tcc: waitpid failed: %s\n", strerror(errno));
            free(python);
            return 1;
        }
    }
    child_pid = -1;
    free(python);

    /* Reclaim the terminal foreground for our own group before exiting. */
    if (interactive) {
        tcsetpgrp(STDIN_FILENO, getpgrp());
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
#endif
}
