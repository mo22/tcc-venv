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
 *   - forwards signals and propagates the child's exit status.
 *
 * The bytes are project-INDEPENDENT (the per-project identity comes from codesign
 * --identifier + the on-disk filename, applied by the installer), so one cached
 * build is copied to every venv.
 *
 * On non-macOS this file is unused — the installer symlinks python-tcc -> python
 * directly — but the code still compiles to a plain exec of the venv python.
 */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

static volatile sig_atomic_t child_pid = -1;

static void forward_signal(int signo) {
    pid_t pid = (pid_t)child_pid;
    if (pid > 0) {
        kill(pid, signo);
    }
}

static void install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = forward_signal;
    sigemptyset(&action.sa_mask);
    int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        sigaction(signals[i], &action, NULL);
    }
}

/* Absolute path to this executable, symlinks resolved. */
static int self_path(char *out, size_t out_len) {
    char raw[PATH_MAX];
#ifdef __APPLE__
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) {
        return -1;
    }
#else
    ssize_t n = readlink("/proc/self/exe", raw, sizeof(raw) - 1);
    if (n < 0) {
        return -1;
    }
    raw[n] = '\0';
#endif
    char resolved[PATH_MAX];
    if (realpath(raw, resolved) == NULL) {
        /* Fall back to the raw path if realpath fails. */
        if (strlen(raw) >= out_len) {
            return -1;
        }
        strcpy(out, raw);
        return 0;
    }
    if (strlen(resolved) >= out_len) {
        return -1;
    }
    strcpy(out, resolved);
    return 0;
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

int main(int argc, char *argv[]) {
    /* self = <venv>/bin/python-tcc-<project>  ->  venv = dirname(dirname(self)) */
    char venv[PATH_MAX];
    if (self_path(venv, sizeof(venv)) != 0) {
        fprintf(stderr, "python-tcc: cannot resolve own executable path\n");
        return 127;
    }
    if (strip_component(venv) != 0 || strip_component(venv) != 0) {
        fprintf(stderr, "python-tcc: unexpected install location (not <venv>/bin/...)\n");
        return 127;
    }

    /* Security: never let a stray $VIRTUAL_ENV redirect us to another venv. */
    const char *venv_env = getenv("VIRTUAL_ENV");
    if (venv_env != NULL && venv_env[0] != '\0') {
        char env_resolved[PATH_MAX];
        if (realpath(venv_env, env_resolved) != NULL && strcmp(env_resolved, venv) != 0) {
            fprintf(
                stderr,
                "python-tcc: refusing to run — $VIRTUAL_ENV (%s) disagrees with my venv (%s)\n",
                env_resolved,
                venv
            );
            return 125;
        }
    }

    char python[PATH_MAX];
    if ((size_t)snprintf(python, sizeof(python), "%s/bin/python", venv) >= sizeof(python)) {
        fprintf(stderr, "python-tcc: venv path too long\n");
        return 127;
    }
    if (access(python, X_OK) != 0) {
        /* Fall back to python3 if a bare `python` is absent. */
        char python3[PATH_MAX];
        if ((size_t)snprintf(python3, sizeof(python3), "%s/bin/python3", venv) < sizeof(python3)
            && access(python3, X_OK) == 0) {
            strcpy(python, python3);
        } else {
            fprintf(stderr, "python-tcc: no venv python at %s\n", python);
            return 127;
        }
    }

    /* child argv: python + argv[1:] */
    char **child_argv = calloc((size_t)argc + 1, sizeof(char *));
    if (child_argv == NULL) {
        fprintf(stderr, "python-tcc: out of memory\n");
        return 127;
    }
    child_argv[0] = python;
    for (int i = 1; i < argc; i++) {
        child_argv[i] = argv[i];
    }
    child_argv[argc] = NULL;

#ifndef __APPLE__
    /* No TCC off-mac: just become python (cheaper, no supervisor needed). */
    execv(python, child_argv);
    fprintf(stderr, "python-tcc: execv %s failed: %s\n", python, strerror(errno));
    free(child_argv);
    return 127;
#else
    pid_t pid;
    int spawn_status = posix_spawn(&pid, python, NULL, NULL, child_argv, environ);
    if (spawn_status != 0) {
        fprintf(stderr, "python-tcc: failed to spawn %s: %s\n", python, strerror(spawn_status));
        free(child_argv);
        return 127;
    }
    free(child_argv);

    child_pid = (sig_atomic_t)pid;
    install_signal_handlers();

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

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
#endif
}
