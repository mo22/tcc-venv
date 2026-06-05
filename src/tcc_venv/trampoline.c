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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>
#endif

extern char **environ;

/* Set by `tcc-venv run`: exec an explicit command (already resolved to an absolute
 * path by the CLI) under our stable identity, instead of our venv python. */
#define EXEC_MODE_ENV "TCC_VENV_EXEC"

#ifdef __APPLE__
/* Private SPI — the same call LLDB uses so the *debuggee* (not the debugger) owns
 * TCC prompts. Setting it on a posix_spawnattr makes the SPAWNED CHILD disclaim our
 * responsibility and become its own TCC-responsible root. We use it to re-spawn a
 * second copy of ourselves that is self-responsible, so OUR stable signed identity —
 * not a non-disclaiming launcher above us (a GUI app, a terminal) — is the
 * responsible root the python grandchild inherits. Returns an errno-style status. */
typedef int (*setdisclaim_fn)(posix_spawnattr_t *, bool);

/* env guard: set by phase A on the re-spawn so phase B skips the bootstrap. */
#define DISCLAIM_GUARD "TCC_VENV_DISCLAIMED"
/* opt-out (debugging / A-B attribution testing): run the old single-process path. */
#define DISCLAIM_OPT_OUT "TCC_VENV_NO_DISCLAIM"
/* sentinel distinct from any real 0..255 exit code. */
#define SUPERVISE_SPAWN_FAILED (-1)
#endif

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

/* Honor $TCC_VENV_CHDIR by changing the working directory the child inherits:
 *   - an absolute path           -> chdir there;
 *   - any other truthy value     -> chdir to the project root (the venv's parent),
 *     so a process launched with an unpredictable cwd (e.g. by a GUI app) still
 *     runs from its repo;
 *   - unset / empty / "0"        -> leave the cwd untouched.
 * A failed chdir is a hard error (return -1): silently running from the wrong
 * directory is worse than refusing — same stance as the $VIRTUAL_ENV guard. */
static int apply_chdir(const char *venv) {
    const char *req = getenv("TCC_VENV_CHDIR");
    if (req == NULL || req[0] == '\0' || strcmp(req, "0") == 0) {
        return 0;
    }
    char *owned = NULL;
    const char *target;
    if (req[0] == '/') {
        target = req;
    } else {
        owned = strdup(venv);
        if (owned == NULL || strip_component(owned) != 0) {
            fprintf(stderr, "python-tcc: cannot derive project root from %s\n", venv);
            free(owned);
            return -1;
        }
        target = owned;
    }
    int rc = chdir(target);
    if (rc != 0) {
        fprintf(stderr, "python-tcc: cannot chdir to %s: %s\n", target, strerror(errno));
    }
    free(owned);
    return rc;
}

#ifdef __APPLE__
/* Spawn `path` (argv `child_argv`) as our own-process-group child, then block +
 * forward signals across the spawn, wait, and return the child's exit code
 * (128 + signo on signal death), or SUPERVISE_SPAWN_FAILED if the spawn failed.
 *   - disclaim_fn != NULL: mark the child to disclaim our TCC responsibility, so it
 *     becomes its own responsible root (used for the A->B re-spawn).
 *   - manage_terminal:     hand the child the controlling terminal's foreground
 *     (used for the final B->python spawn so an interactive REPL / Ctrl-C works). */
static int spawn_and_supervise(const char *path, char **child_argv,
                               setdisclaim_fn disclaim_fn, int manage_terminal) {
    /* Don't let us be stopped if we write to the tty while the child owns the
     * terminal foreground (set below). */
    signal(SIGTTOU, SIG_IGN);

    /* Block the forwarded signals across spawn + child_pid assignment so a signal
     * arriving before the handlers are live can't kill only us and orphan the
     * child (the handlers are a no-op until child_pid is valid). */
    sigset_t block, prev;
    sigemptyset(&block);
    for (int i = 0; i < N_FORWARDED; i++) {
        sigaddset(&block, FORWARDED_SIGNALS[i]);
    }
    sigprocmask(SIG_BLOCK, &block, &prev);

    /* Reset the child's mask to the pre-block set (SETSIGMASK) and dispositions to
     * default (SETSIGDEF) — otherwise it inherits our temporarily-blocked mask and
     * never sees a forwarded SIGTERM. */
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
    if (disclaim_fn != NULL && disclaim_fn(&attr, true) != 0) {
        /* Couldn't arrange self-responsibility — don't spawn a pointless extra
         * layer; let the caller fall through to running python directly. */
        posix_spawnattr_destroy(&attr);
        sigprocmask(SIG_SETMASK, &prev, NULL);
        fprintf(stderr, "python-tcc: disclaim setup failed; running without bootstrap\n");
        return SUPERVISE_SPAWN_FAILED;
    }

    pid_t pid;
    int spawn_status = posix_spawn(&pid, path, NULL, &attr, child_argv, environ);
    posix_spawnattr_destroy(&attr);
    if (spawn_status != 0) {
        sigprocmask(SIG_SETMASK, &prev, NULL);
        fprintf(stderr, "python-tcc: failed to spawn %s: %s\n", path, strerror(spawn_status));
        return SUPERVISE_SPAWN_FAILED;
    }

    child_pid = (sig_atomic_t)pid;

    /* Hand the controlling terminal's foreground to the child's group so an
     * interactive REPL / input() keeps working and Ctrl-C reaches the child. */
    int interactive = manage_terminal && isatty(STDIN_FILENO);
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
            return 1;
        }
    }
    child_pid = -1;

    /* Reclaim the terminal foreground for our own group before returning. */
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
}
#endif /* __APPLE__ */

int main(int argc, char *argv[]) {
    /* self = <venv>/bin/python-tcc-<project>  ->  venv = dirname(dirname(self)) */
    char *self = self_path();
    if (self == NULL) {
        fprintf(stderr, "python-tcc: cannot resolve own executable path\n");
        return 127;
    }

#ifdef __APPLE__
    /* Phase A: re-spawn ourselves as a TCC-self-responsible process BEFORE we touch
     * python, so our stable signed identity — not a non-disclaiming launcher above us
     * (a GUI app, a terminal) — is the responsible root the python child
     * inherits. Skipped if already disclaimed (guard), opted out, or if the private
     * disclaim SPI is unavailable (degrade to the launchd-only path, which is correct
     * when the parent already self-responsibilizes). */
    if (getenv(DISCLAIM_GUARD) == NULL && getenv(DISCLAIM_OPT_OUT) == NULL) {
        setdisclaim_fn disclaim_fn =
            (setdisclaim_fn)dlsym(RTLD_DEFAULT, "responsibility_spawnattrs_setdisclaim");
        if (disclaim_fn != NULL) {
            /* child argv = [self, argv[1..]] (argc may be 0 in a pathological exec). */
            int passthrough = argc > 1 ? argc - 1 : 0;
            char **self_argv = calloc((size_t)passthrough + 2, sizeof(char *));
            if (self_argv != NULL) {
                self_argv[0] = self;
                for (int i = 0; i < passthrough; i++) {
                    self_argv[i + 1] = argv[i + 1];
                }
                self_argv[passthrough + 1] = NULL;
                /* Guard the re-spawn against infinite recursion. If we can't even set
                 * the env, do NOT spawn (the child would loop) — fall through. */
                if (setenv(DISCLAIM_GUARD, "1", 1) == 0) {
                    int rc = spawn_and_supervise(self, self_argv, disclaim_fn,
                                                 /*manage_terminal=*/1);
                    free(self_argv);
                    if (rc != SUPERVISE_SPAWN_FAILED) {
                        free(self);
                        return rc;
                    }
                    /* Self-spawn failed: fall through and run python directly. */
                    unsetenv(DISCLAIM_GUARD);
                } else {
                    free(self_argv);
                }
            }
        }
    }
    /* Phase B: we are self-responsible now. Don't leak the guard into python — a
     * nested python-tcc the app launches must run its own bootstrap. */
    unsetenv(DISCLAIM_GUARD);
#endif

    /* Phase B (self-responsible) or the non-disclaim fallback: resolve the venv and
     * run its python directly. */
    char *venv = strdup(self);
    free(self);
    if (venv == NULL) {
        fprintf(stderr, "python-tcc: out of memory\n");
        return 127;
    }
    if (strip_component(venv) != 0 || strip_component(venv) != 0) {
        fprintf(stderr, "python-tcc: unexpected install location (not <venv>/bin/...)\n");
        free(venv);
        return 127;
    }

    /* Generic-exec mode (set by `tcc-venv run`): exec an explicit command under our
     * stable, self-responsible identity instead of our venv python. argv[1] is the
     * program (the CLI already resolved it to an absolute path). The $VIRTUAL_ENV
     * guard below is skipped — the command is explicit, not interpreter-selection.
     * Consume the env so it never leaks into the child. */
    const char *exec_req = getenv(EXEC_MODE_ENV);
    int exec_mode = exec_req != NULL && exec_req[0] != '\0' && strcmp(exec_req, "0") != 0;
    unsetenv(EXEC_MODE_ENV);
    if (exec_mode) {
        if (argc < 2) {
            fprintf(stderr, "python-tcc: %s set but no command given\n", EXEC_MODE_ENV);
            free(venv);
            return 127;
        }
        if (apply_chdir(venv) != 0) {
            free(venv);
            return 127;
        }
        free(venv);
#ifndef __APPLE__
        execv(argv[1], &argv[1]);
        fprintf(stderr, "python-tcc: execv %s failed: %s\n", argv[1], strerror(errno));
        return 127;
#else
        int rc = spawn_and_supervise(argv[1], &argv[1], NULL, /*manage_terminal=*/1);
        return rc == SUPERVISE_SPAWN_FAILED ? 127 : rc;
#endif
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

    /* Optionally set the cwd the child inherits (project root or an explicit path).
     * Done after python is resolved (absolute) so it's unaffected by the chdir. */
    if (apply_chdir(venv) != 0) {
        free(python);
        free(venv);
        return 127;
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
    /* We are self-responsible here (phase B re-spawned by phase A with disclaim, or
     * launched directly under a parent that already self-responsibilizes us). Spawn
     * python WITHOUT disclaim so it inherits our stable identity, and hand it the
     * controlling terminal so an interactive REPL works. */
    int rc = spawn_and_supervise(python, child_argv, /*disclaim_fn=*/NULL,
                                 /*manage_terminal=*/1);
    free(child_argv);
    free(python);
    if (rc == SUPERVISE_SPAWN_FAILED) {
        return 127;
    }
    return rc;
#endif
}
