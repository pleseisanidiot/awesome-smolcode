#define _POSIX_C_SOURCE 200809L
#include "smolcode.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static Buf error(const char *what)
{
    Buf b = {0}; buf_puts(&b, "Error: "); buf_puts(&b, what); return b;
}

static Buf io_error(const char *what)
{
    Buf b = {0}; buf_printf(&b, "Error: %s: %s", what, strerror(errno)); return b;
}

static int line_number(const Json *j, size_t token, size_t *value)
{
    if (token == J_NONE) return 1;
    if (j->tokens[token].type != J_PRIMITIVE) return 0;
    size_t n = 0;
    for (size_t i = j->tokens[token].start; i < j->tokens[token].end; i++) {
        unsigned char c = (unsigned char)j->text[i];
        if (c < '0' || c > '9' || n > (SIZE_MAX - (c - '0')) / 10) return 0;
        n = n * 10 + (c - '0');
    }
    if (!n) return 0;
    *value = n; return 1;
}

static Buf read_file(const char *path, size_t first, size_t last, int ranged)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return io_error("open for reading");
    struct stat st;
    if (fstat(fd, &st) < 0) { Buf e = io_error("stat"); close(fd); return e; }
    if (!S_ISREG(st.st_mode)) { close(fd); return error("read requires a regular file"); }
    if (st.st_size < 0 || (!ranged && (uintmax_t)st.st_size > MAX_TOOL_OUTPUT)) {
        close(fd); return error("file exceeds MAX_TOOL_OUTPUT; use read with start_line/end_line; no contents returned");
    }
    Buf b = {0}; char chunk[8192]; ssize_t n; size_t line = 1; int done = 0;
    while (!interrupted && !done && (n = read(fd, chunk, sizeof(chunk))) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            Buf e = io_error("read"); close(fd); buf_free(&b); return e;
        }
        for (size_t i = 0; i < (size_t)n;) {
            char *newline = memchr(chunk + i, '\n', (size_t)n - i);
            size_t take = newline ? (size_t)(newline - chunk) + 1 - i : (size_t)n - i;
            if (line >= first) {
                if (take > MAX_TOOL_OUTPUT - b.len) {
                    close(fd); buf_free(&b);
                    return error("selected contents exceed MAX_TOOL_OUTPUT; request fewer lines; no contents returned");
                }
                buf_add(&b, chunk + i, take);
            }
            i += take;
            if (newline) { if (line == last) { done = 1; break; } line++; }
        }
    }
    close(fd);
    if (interrupted) { buf_free(&b); return error("read interrupted"); }
    if (ranged && !b.len) return error("start_line is beyond end of file");
    buf_add(&b, "", 0);
    return b;
}

static Buf write_file(const char *path, const Buf *content)
{
    /* Check the target before truncating; reject devices, directories and FIFOs. */
    int fd = open(path, O_WRONLY | O_CREAT | O_NONBLOCK | O_CLOEXEC, 0666);
    if (fd < 0) return io_error("open for writing");
    struct stat st;
    if (fstat(fd, &st) < 0) { Buf e = io_error("stat"); close(fd); return e; }
    if (!S_ISREG(st.st_mode)) { close(fd); return error("write requires a regular file"); }
    if (ftruncate(fd, 0) < 0) { Buf e = io_error("truncate"); close(fd); return e; }
    size_t done = 0;
    while (done < content->len && !interrupted) {
        size_t n = content->len - done;
        if (n > 65536) n = 65536;
        ssize_t wrote = write(fd, content->data + done, n);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) {
            if (!wrote) errno = EIO;
            Buf e = io_error("write (file may be partial)"); close(fd); return e;
        }
        done += (size_t)wrote;
    }
    if (close(fd) < 0) return io_error("close after writing (file may be partial)");
    if (interrupted) return error("write interrupted; file may be partial");
    Buf b = {0}; buf_printf(&b, "Wrote %zu bytes.", done); return b;
}

static double now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) {
        fputs("smolcode: monotonic clock unavailable\n", stderr); exit(EXIT_FAILURE);
    }
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static Buf shell(const char *command)
{
    int fds[2];
    /* Reap grandchildren when a shell exits before its background jobs. */
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) return io_error("enable child reaping");
    if (pipe(fds) < 0) return io_error("pipe");
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) {
        Buf e = io_error("configure pipe"); close(fds[0]); close(fds[1]); return e;
    }
    pid_t pid = fork();
    if (pid < 0) { Buf e = io_error("fork"); close(fds[0]); close(fds[1]); return e; }
    if (!pid) {
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        if (setpgid(0, 0) < 0) _exit(126);
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) _exit(126);
        int input = open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0 ||
            dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0) _exit(126);
        if (input > STDERR_FILENO) close(input);
        close(fds[0]); close(fds[1]);
        /* Credentials are used by HTTPS, never inherited by model commands. */
        if (unsetenv(API_KEY_ENV) < 0) _exit(126);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        perror("/bin/sh"); _exit(127);
    }
    close(fds[1]);
    (void)setpgid(pid, pid); /* The child also sets this before executing. */
    Buf output = {0};
    int status = 0, reaped = 0, have_status = 0, eof = 0, stopped = 0, killed = 0;
    int timed_out = 0, cancelled = 0, truncated = 0, failed = 0;
    double deadline = now() + TOOL_TIMEOUT_SECONDS, stop_time = 0;
    for (;;) {
        /* Bound each drain so a continuously writing child cannot evade timeout. */
        for (int i = 0; i < 16 && !eof; i++) {
            char chunk[8192]; ssize_t n = read(fds[0], chunk, sizeof(chunk));
            if (n == 0) { eof = 1; break; }
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) { failed = 1; eof = 1; }
                break;
            }
            size_t take = (size_t)n;
            if (take > MAX_TOOL_OUTPUT - output.len) { take = MAX_TOOL_OUTPUT - output.len; truncated = 1; }
            buf_add(&output, chunk, take);
        }
        if (!reaped) {
            pid_t p = waitpid(pid, &status, WNOHANG);
            if (p == pid) { reaped = 1; have_status = 1; }
            else if (p < 0 && errno != EINTR) { failed = 1; reaped = 1; }
        }
        double t = now();
        if (!stopped && (interrupted || t >= deadline || reaped || failed)) {
            cancelled = interrupted != 0;
            timed_out = !reaped && t >= deadline;
            stopped = 1; stop_time = t;
            (void)kill(-pid, SIGTERM);
        }
        if (stopped && !killed && t - stop_time >= 0.2) {
            (void)kill(-pid, SIGKILL); killed = 1;
        }
        if (stopped && reaped && eof && (killed || (kill(-pid, 0) < 0 && errno == ESRCH))) break;
        if (killed && t - stop_time >= 0.3) break; /* A deliberately detached child may retain the pipe. */
        struct pollfd pfd = {.fd = eof ? -1 : fds[0], .events = POLLIN};
        if (poll(&pfd, 1, 50) < 0 && errno != EINTR) failed = 1;
    }
    close(fds[0]);
    if (!reaped) {
        pid_t p;
        do { p = waitpid(pid, &status, 0); } while (p < 0 && errno == EINTR);
        if (p == pid) have_status = 1;
        else failed = 1;
    }
    /* All ordinary descendants stay in the command's group. */
    while (waitpid(-pid, NULL, 0) > 0 || errno == EINTR) {}
    Buf result = {0};
    if (failed) buf_puts(&result, "Error: shell capture or wait failed.\n");
    if (timed_out) buf_printf(&result, "Timed out after %d seconds.\n", TOOL_TIMEOUT_SECONDS);
    if (cancelled) buf_puts(&result, "Interrupted.\n");
    if (!have_status) buf_puts(&result, "Exit status unavailable.\n");
    else if (WIFEXITED(status)) buf_printf(&result, "Exit status: %d\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) buf_printf(&result, "Terminated by signal: %d\n", WTERMSIG(status));
    else buf_puts(&result, "Exit status unavailable.\n");
    buf_add(&result, output.data, output.len);
    if (truncated) buf_printf(&result, "\n[Output truncated at %d bytes.]\n", MAX_TOOL_OUTPUT);
    buf_free(&output);
    return result;
}

Buf tool_execute(const char *name, const char *arguments, size_t len)
{
    Json j = {0}; Buf path = {0}, content = {0}, result = {0};
    if (interrupted) return error("not executed: operation interrupted");
    if (strcmp(name, "read") && strcmp(name, "write") && strcmp(name, "sh"))
        return error("unknown tool; only read, write, and sh exist");
    if (!json_parse(&j, arguments, len)) return error("invalid JSON arguments");
    int is_sh = !strcmp(name, "sh"), is_write = !strcmp(name, "write");
    size_t first = 1, last = SIZE_MAX;
    size_t start = is_sh || is_write ? J_NONE : json_get(&j, 0, "start_line");
    size_t end = is_sh || is_write ? J_NONE : json_get(&j, 0, "end_line");
    int ranged = start != J_NONE || end != J_NONE;
    if (j.tokens[0].type != J_OBJECT || j.tokens[0].count != (is_write ? 2u : 1u) +
        (start != J_NONE) + (end != J_NONE) ||
        !json_string(&j, json_get(&j, 0, is_sh ? "command" : "path"), &path) ||
        !path.len || memchr(path.data, '\0', path.len) ||
        (is_write && !json_string(&j, json_get(&j, 0, "content"), &content))) {
        result = error("expected string path (and content for write), or string command for sh; no extra keys");
    } else if (!line_number(&j, start, &first) || !line_number(&j, end, &last) || last < first) {
        result = error("start_line/end_line must be positive integers with end_line >= start_line");
    } else {
        Buf activity = {0}; buf_add(&activity, path.data, path.len); redact(&activity);
        for (size_t i = 0; i < activity.len; i++)
            if ((unsigned char)activity.data[i] < 0x20 || activity.data[i] == 0x7f) activity.data[i] = ' ';
        printf("\342\227\217 %s ", name);
        safe_print(activity.data, activity.len > 120 ? 120 : activity.len);
        if (activity.len > 120) fputs("...", stdout);
        if (ranged) {
            printf(" [lines %zu-", first);
            if (end == J_NONE) fputs("EOF]", stdout); else printf("%zu]", last);
        }
        putchar('\n'); fflush(stdout); buf_free(&activity);
        if (is_sh) result = shell(path.data);
        else if (is_write) result = write_file(path.data, &content);
        else result = read_file(path.data, first, last, ranged);
    }
    json_free(&j); buf_free(&path); buf_free(&content); redact(&result);
    return result;
}
