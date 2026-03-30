/*
 * pty-proxy — PTY proxy for Claude Code message injection
 *
 * Sits between the terminal and a child process (Claude Code),
 * forwarding I/O bidirectionally. Also listens on a Unix domain
 * socket for injected messages that appear as user input.
 *
 * Usage: pty-proxy --socket /tmp/urho_claude/tty/coder3.sock -- claude "prompt"
 *
 * Build: gcc -o pty-proxy pty-proxy.c -lutil
 */

#define _XOPEN_SOURCE 600
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <time.h>

#define BUF_SIZE 4096
#define MAX_INJECT_CLIENTS 8
#define INJECT_BUF_SIZE 8192
#define DEATH_LOG "/tmp/urho_claude/pty-proxy-deaths.log"

static volatile sig_atomic_t child_exited = 0;
static volatile sig_atomic_t winch_received = 0;
static pid_t child_pid = 0;
static int master_fd = -1;
static struct termios orig_termios;
static int termios_saved = 0;

static const char *g_socket_path = NULL;

static void log_death(const char *reason, int extra) {
    FILE *f = fopen(DEATH_LOG, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    fprintf(f, "[%s] pid=%d child=%d socket=%s reason=%s extra=%d\n",
            ts, getpid(), child_pid, g_socket_path ? g_socket_path : "?", reason, extra);
    fclose(f);
}

static void sigchld_handler(int sig) {
    (void)sig;
    child_exited = 1;
}

static void sigwinch_handler(int sig) {
    (void)sig;
    winch_received = 1;
}

static void restore_termios(void) {
    if (termios_saved)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void cleanup_socket(const char *path) {
    if (path)
        unlink(path);
}

/* Forward terminal window size to the child PTY */
static void forward_winsize(void) {
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
        ioctl(master_fd, TIOCSWINSZ, &ws);
}

/* Create and bind a Unix domain socket, return fd or -1 */
static int create_listen_socket(const char *path) {
    struct sockaddr_un addr;
    int fd;

    /* Ensure parent directory exists */
    char *dir = strdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0700);  /* best effort */
    }
    free(dir);

    /* Remove stale socket */
    unlink(path);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 4) < 0) {
        perror("listen");
        close(fd);
        unlink(path);
        return -1;
    }

    /* Allow group access */
    chmod(path, 0770);

    return fd;
}

/* Set fd to non-blocking */
static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s --socket <path> -- <command> [args...]\n", prog);
    exit(1);
}

int main(int argc, char *argv[]) {
    const char *socket_path = NULL;
    int cmd_start = -1;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--") == 0) {
            cmd_start = i + 1;
            break;
        }
    }

    if (!socket_path || cmd_start < 0 || cmd_start >= argc)
        usage(argv[0]);

    g_socket_path = socket_path;

    /* Save original terminal settings */
    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &orig_termios) == 0)
            termios_saved = 1;
    }

    /* Get current window size */
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0) {
        ws.ws_row = 24;
        ws.ws_col = 80;
    }

    /* Create PTY and fork */
    child_pid = forkpty(&master_fd, NULL, NULL, &ws);
    if (child_pid < 0) {
        perror("forkpty");
        exit(1);
    }

    if (child_pid == 0) {
        /* Child: export socket path so hooks can create role symlinks */
        setenv("PTY_PROXY_SOCK", socket_path, 1);
        execvp(argv[cmd_start], &argv[cmd_start]);
        perror("execvp");
        _exit(127);
    }

    /* Parent: set up signal handlers */
    signal(SIGCHLD, sigchld_handler);
    signal(SIGWINCH, sigwinch_handler);
    atexit(restore_termios);

    /* Put terminal in raw mode so keystrokes pass through */
    if (termios_saved) {
        struct termios raw = orig_termios;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }

    set_nonblocking(master_fd);
    set_nonblocking(STDIN_FILENO);

    /* Create injection socket */
    int listen_fd = create_listen_socket(socket_path);
    if (listen_fd < 0) {
        fprintf(stderr, "Warning: injection socket failed, running without it\n");
    }

    /* Client connections for injection */
    int client_fds[MAX_INJECT_CLIENTS];
    int num_clients = 0;
    for (int i = 0; i < MAX_INJECT_CLIENTS; i++)
        client_fds[i] = -1;

    /* Injection queue: messages waiting to be written to master_fd */
    char inject_buf[INJECT_BUF_SIZE];
    int inject_len = 0;

    /* Fixed poll array — never use alloca() inside a loop (stack overflow) */
    struct pollfd pfds[3 + MAX_INJECT_CLIENTS];

    /* Main I/O loop */
    while (!child_exited) {
        /* Handle window resize */
        if (winch_received) {
            winch_received = 0;
            forward_winsize();
        }

        /* Build poll set */
        /* slots: 0=stdin, 1=master, 2=listen, 3..N=clients */
        int nfds = 3 + num_clients;

        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;

        pfds[1].fd = master_fd;
        pfds[1].events = POLLIN;
        /* If we have injection data queued, also poll for write-ready */
        if (inject_len > 0)
            pfds[1].events |= POLLOUT;
        pfds[1].revents = 0;

        pfds[2].fd = listen_fd;
        pfds[2].events = (listen_fd >= 0) ? POLLIN : 0;
        pfds[2].revents = 0;

        int ci = 0;
        for (int i = 0; i < MAX_INJECT_CLIENTS; i++) {
            if (client_fds[i] >= 0) {
                pfds[3 + ci].fd = client_fds[i];
                pfds[3 + ci].events = POLLIN;
                pfds[3 + ci].revents = 0;
                ci++;
            }
        }
        nfds = 3 + ci;

        int ret = poll(pfds, nfds, 100);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            log_death("poll_error", errno);
            break;
        }

        /* stdin → master (user typing) */
        if (pfds[0].revents & POLLIN) {
            char buf[BUF_SIZE];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                /* Write to master (blocking write is fine for small user input) */
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(master_fd, buf + written, n - written);
                    if (w < 0) {
                        if (errno == EAGAIN || errno == EINTR)
                            continue;
                        break;
                    }
                    written += w;
                }
            } else if (n == 0) {
                log_death("stdin_closed", 0);
                break;  /* stdin closed */
            }
        }

        /* master → stdout (child output) */
        if (pfds[1].revents & POLLIN) {
            char buf[BUF_SIZE];
            ssize_t n = read(master_fd, buf, sizeof(buf));
            if (n > 0) {
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(STDOUT_FILENO, buf + written, n - written);
                    if (w < 0) {
                        if (errno == EAGAIN || errno == EINTR)
                            continue;
                        break;
                    }
                    written += w;
                }
            } else if (n == 0) {
                log_death("master_closed", 0);
                break;  /* child closed */
            }
        }

        /* Flush injection buffer to master — only when Claude is idle (ready file exists) */
        if ((pfds[1].revents & POLLOUT) && inject_len > 0) {
            /* Check for .ready gate file (Stop hook creates it when Claude is idle) */
            char ready_path[256];
            {
                /* Derive role from socket path: /tmp/.../planner.sock → planner */
                const char *base = strrchr(g_socket_path, '/');
                base = base ? base + 1 : g_socket_path;
                char role[64];
                strncpy(role, base, sizeof(role) - 1);
                role[sizeof(role) - 1] = '\0';
                char *dot = strrchr(role, '.');
                if (dot) *dot = '\0';
                snprintf(ready_path, sizeof(ready_path), "/tmp/urho_claude/tty/%s.ready", role);
            }

            struct stat rst;
            if (stat(ready_path, &rst) == 0) {
                /* Claude is idle — flush content, hold trailing \r for next cycle */
                int flush_len = inject_len;
                int has_trailing_cr = (inject_len > 0 && inject_buf[inject_len - 1] == '\r');
                if (has_trailing_cr)
                    flush_len--;  /* hold the \r */

                if (flush_len > 0) {
                    ssize_t w = write(master_fd, inject_buf, flush_len);
                    if (w > 0) {
                        if (w < flush_len)
                            memmove(inject_buf, inject_buf + w, inject_len - w);
                        else if (has_trailing_cr) {
                            inject_buf[0] = '\r';
                            inject_len = 1;
                        } else
                            inject_len = 0;
                        if (!has_trailing_cr)
                            inject_len -= w;
                    }
                }
                unlink(ready_path);
            } else if (inject_len == 1 && inject_buf[0] == '\r') {
                /* Trailing \r from previous flush — send it now (next poll cycle) */
                write(master_fd, inject_buf, 1);
                inject_len = 0;
            }
            /* If no .ready file and not a lone \r, hold data until next cycle */
        }

        /* Accept new injection clients */
        if (pfds[2].revents & POLLIN) {
            int cfd = accept(listen_fd, NULL, NULL);
            if (cfd >= 0) {
                set_nonblocking(cfd);
                /* Find a slot */
                int placed = 0;
                for (int i = 0; i < MAX_INJECT_CLIENTS; i++) {
                    if (client_fds[i] < 0) {
                        client_fds[i] = cfd;
                        if (i >= num_clients) num_clients = i + 1;
                        placed = 1;
                        break;
                    }
                }
                if (!placed)
                    close(cfd);  /* no room */
            }
        }

        /* Read from injection clients */
        ci = 0;
        for (int i = 0; i < MAX_INJECT_CLIENTS; i++) {
            if (client_fds[i] < 0)
                continue;
            if (ci + 3 < nfds && pfds[3 + ci].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buf[BUF_SIZE];
                ssize_t n = read(client_fds[i], buf, sizeof(buf));
                if (n > 0) {
                    /* Append to injection buffer (drop if full) */
                    int space = INJECT_BUF_SIZE - inject_len;
                    int copy = (n < space) ? n : space;
                    if (copy > 0) {
                        memcpy(inject_buf + inject_len, buf, copy);
                        inject_len += copy;
                    }
                } else if (n <= 0) {
                    /* Client disconnected or error */
                    close(client_fds[i]);
                    client_fds[i] = -1;
                }
            }
            ci++;
        }

        /* Recompute num_clients */
        num_clients = 0;
        for (int i = MAX_INJECT_CLIENTS - 1; i >= 0; i--) {
            if (client_fds[i] >= 0) {
                num_clients = i + 1;
                break;
            }
        }

        /* Check if child is done */
        if (pfds[1].revents & POLLHUP) {
            log_death("master_hup", 0);
            break;
        }
    }

    /* Log if we exited due to SIGCHLD */
    if (child_exited)
        log_death("sigchld", 0);

    /* Clean up */
    restore_termios();
    termios_saved = 0;  /* prevent double-restore from atexit */

    if (listen_fd >= 0)
        close(listen_fd);
    cleanup_socket(socket_path);

    for (int i = 0; i < MAX_INJECT_CLIENTS; i++) {
        if (client_fds[i] >= 0)
            close(client_fds[i]);
    }

    /* Wait for child and propagate exit code */
    int status = 0;
    waitpid(child_pid, &status, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        log_death("child_exited", code);
        return code;
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        log_death("child_signaled", sig);
        return 128 + sig;
    }
    log_death("child_unknown_status", status);
    return 1;
}
