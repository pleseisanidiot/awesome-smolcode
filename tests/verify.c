#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "smolcode.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t interrupted;
static unsigned checks;
static int connection;
static void file_put(const char *path, const char *data, size_t len);
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void handler(int sig) { (void)sig; interrupted = 1; }

static void pause_ms(long ms)
{
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}

static int has(const Buf *b, const char *s) { return b->data && strstr(b->data, s) != NULL; }

static Buf tool(const char *name, const char *args) { return tool_execute(name, args, strlen(args)); }

static void expect_tool(const char *name, const char *args, const char *expected)
{
    Buf b = tool(name, args); CHECK(has(&b, expected)); buf_free(&b);
}

static void test_json(void)
{
    Json j = {0}; Buf b = {0}, q = {0};
    const char *s = "{\"p\\u0061th\":\"a\\n\\t\\r\\b\\f\\/\\\\\\\"\\u0000\\uD83D\\uDE00\",\"nested\":[{},[1,-2.5e+3],true,null]}";
    CHECK(json_parse(&j, s, strlen(s)));
    CHECK(json_string(&j, json_get(&j, 0, "path"), &b));
    const char want[] = "a\n\t\r\b\f/\\\"\0\xf0\x9f\x98\x80";
    CHECK(b.len == sizeof(want) - 1 && !memcmp(b.data, want, b.len));
    json_quote(&q, b.data, b.len); json_free(&j);
    CHECK(json_parse(&j, q.data, q.len)); Buf round = {0};
    CHECK(json_string(&j, 0, &round)); CHECK(round.len == b.len && !memcmp(round.data, b.data, b.len));
    json_free(&j); buf_free(&b); buf_free(&q); buf_free(&round);
    const char *bad[] = {"", "{", "[1,]", "{\"a\":1,}", "01", ".1", "1e", "+1", "--1", "true false",
        "\"\\uD800\"", "\"\\uDC00\"", "\"\\x01\"", "\"\n\"", "[", "{1:2}", "\"\xc0\x80\""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) CHECK(!json_parse(&j, bad[i], strlen(bad[i])));
    CHECK(json_parse(&j, "{\"a\":1,\"a\":2}", 13)); CHECK(json_get(&j, 0, "a") == J_NONE); json_free(&j);
    char deep[401]; memset(deep, '[', 200); memset(deep + 200, ']', 200); deep[400] = 0;
    CHECK(!json_parse(&j, deep, 400));
    unsigned seed = 7;
    for (unsigned i = 0; i < 2000; i++) {
        char random[64];
        for (size_t k = 0; k < sizeof(random); k++) { seed = seed * 1664525u + 1013904223u; random[k] = (char)(seed >> 24); }
        (void)json_parse(&j, random, sizeof(random)); json_free(&j);
    }
    puts("PASS JSON escapes, Unicode, NUL, malformed input, depth and randomized parsing");
}

static void test_tools(void)
{
    expect_tool("read", "{\"path\":\"missing.c\"}", "Error:");
    expect_tool("write", "{\"path\":\"file.c\",\"content\":\"first\\n\\\"quote\\\" \\uD83D\\uDE00 %s\\n\\u0000tail\"}", "Wrote");
    Buf b = tool("read", "{\"path\":\"file.c\"}");
    const char exact[] = "first\n\"quote\" \xf0\x9f\x98\x80 %s\n\0tail";
    CHECK(b.len == sizeof(exact) - 1 && !memcmp(b.data, exact, b.len)); buf_free(&b);
    b = tool("read", "{\"path\":\"file.c\",\"start_line\":2,\"end_line\":2}");
    const char second[] = "\"quote\" \xf0\x9f\x98\x80 %s\n";
    CHECK(b.len == sizeof(second) - 1 && !memcmp(b.data, second, b.len)); buf_free(&b);
    b = tool("read", "{\"path\":\"file.c\",\"start_line\":3}");
    CHECK(b.len == 5 && !memcmp(b.data, "\0tail", 5)); buf_free(&b);
    b = tool("read", "{\"path\":\"file.c\",\"end_line\":1}");
    CHECK(b.len == 6 && !memcmp(b.data, "first\n", 6)); buf_free(&b);
    expect_tool("read", "{\"path\":\"file.c\",\"start_line\":4}", "beyond end of file");
    const char *invalid_ranges[] = {
        "\"start_line\":0", "\"start_line\":-1", "\"end_line\":0", "\"start_line\":1.5",
        "\"start_line\":\"1\"", "\"start_line\":null", "\"start_line\":true",
        "\"start_line\":184467440737095516160", "\"start_line\":3,\"end_line\":2"
    };
    for (size_t i = 0; i < sizeof(invalid_ranges) / sizeof(*invalid_ranges); i++) {
        Buf bad = {0}; buf_printf(&bad, "{\"path\":\"file.c\",%s}", invalid_ranges[i]);
        b = tool_execute("read", bad.data, bad.len); CHECK(has(&b, "positive integers"));
        buf_free(&b); buf_free(&bad);
    }
    expect_tool("read", "{\"path\":\"file.c\",\"start_line\":1,\"extra\":2}", "no extra keys");
    expect_tool("read", "{\"path\":\"file.c\",\"start_line\":1,\"start_line\":2}", "no extra keys");
    expect_tool("write", "{\"path\":\"file.c\",\"content\":\"new\"}", "Wrote 3 bytes");
    b = tool("read", "{\"path\":\"file.c\"}"); CHECK(b.len == 3 && !memcmp(b.data, "new", 3)); buf_free(&b);
    expect_tool("write", "{\"path\":\"empty\",\"content\":\"\"}", "Wrote 0 bytes");
    b = tool("read", "{\"path\":\"empty\"}"); CHECK(b.len == 0); buf_free(&b);
    expect_tool("read", "{\"path\":\"empty\",\"start_line\":1}", "beyond end of file");
    int fd = open("large", O_CREAT | O_WRONLY, 0600); CHECK(fd >= 0);
    CHECK(ftruncate(fd, MAX_TOOL_OUTPUT + 1) == 0); CHECK(close(fd) == 0);
    expect_tool("read", "{\"path\":\"large\"}", "no contents returned");
    Buf args = {0}; buf_puts(&args, "{\"path\":\"huge-write\",\"content\":\"");
    for (size_t i = 0; i < 1024 * 1024; i++) buf_puts(&args, "x");
    buf_puts(&args, "\"}"); b = tool_execute("write", args.data, args.len);
    CHECK(has(&b, "Wrote 1048576 bytes")); buf_free(&b); buf_free(&args);
    struct stat st; CHECK(stat("huge-write", &st) == 0 && st.st_size == 1048576);
    expect_tool("read", "{\"path\":\"huge-write\",\"start_line\":1,\"end_line\":1}", "no contents returned");
    Buf fixture = {0};
    for (size_t i = 0; i < 9000; i++) buf_puts(&fixture, "x");
    buf_puts(&fixture, "\ntwo\r\nthree"); file_put("chunks", fixture.data, fixture.len); buf_free(&fixture);
    b = tool("read", "{\"path\":\"chunks\",\"start_line\":2,\"end_line\":10}");
    CHECK(b.len == 10 && !memcmp(b.data, "two\r\nthree", 10)); buf_free(&b);
    b = tool("read", "{\"path\":\"chunks\",\"end_line\":1}"); CHECK(b.len == 9001 && b.data[9000] == '\n'); buf_free(&b);
    for (size_t i = 0; i < 40000; i++) buf_puts(&fixture, "x\n");
    buf_puts(&fixture, "tail"); file_put("chunks", fixture.data, fixture.len); buf_free(&fixture);
    expect_tool("read", "{\"path\":\"chunks\"}", "no contents returned");
    b = tool("read", "{\"path\":\"chunks\",\"start_line\":40001}");
    CHECK(b.len == 4 && !memcmp(b.data, "tail", 4)); buf_free(&b);
    for (size_t i = 0; i < MAX_TOOL_OUTPUT - 1; i++) buf_puts(&fixture, "x");
    buf_puts(&fixture, "\ntail\n"); file_put("chunks", fixture.data, fixture.len); buf_free(&fixture);
    b = tool("read", "{\"path\":\"chunks\",\"end_line\":1}");
    CHECK(b.len == MAX_TOOL_OUTPUT && b.data[b.len - 1] == '\n'); buf_free(&b);
    expect_tool("read", "{\"path\":\"chunks\",\"end_line\":2}", "no contents returned");
    b = tool("read", "{\"path\":\"chunks\",\"start_line\":2}");
    CHECK(b.len == 5 && !memcmp(b.data, "tail\n", 5)); buf_free(&b);
    CHECK(mkfifo("fifo", 0600) == 0);
    expect_tool("read", "{\"path\":\"fifo\"}", "regular file");
    expect_tool("write", "{\"path\":\"no-parent/file\",\"content\":\"x\"}", "Error:");
    expect_tool("write", "{\"path\":\".\",\"content\":\"x\"}", "Error:");
    expect_tool("read", "{\"path\":12}", "expected string");
    expect_tool("read", "{\"path\":\"file.c\\u0000other\"}", "expected string");
    expect_tool("read", "{\"path\":\"file.c\",\"extra\":1}", "expected string");
    expect_tool("read", "{\"path\":\"file.c\",\"path\":\"other\"}", "expected string");
    expect_tool("read", "{bad", "invalid JSON"); expect_tool("fourth", "{}", "unknown tool");
    expect_tool("sh", "{\"command\":\"printf 'stdout\\n'; printf 'stderr\\n' >&2\"}", "Exit status: 0\nstdout\nstderr\n");
    expect_tool("sh", "{\"command\":\"printf failure >&2; exit 7\"}", "Exit status: 7\nfailure");
    expect_tool("sh", "{\"command\":\"command-does-not-exist-smolcode\"}", "Exit status: 127");
    b = tool("sh", "{\"command\":\"yes x | head -c 70000\"}");
    CHECK(has(&b, "Exit status: 0")); CHECK(has(&b, "Output truncated at 65536 bytes")); buf_free(&b);
    expect_tool("sh", "{\"command\":\"trap '' TERM; sleep 30\"}", "Timed out after 1 seconds");
    b = tool("sh", "{\"command\":\"sleep 30 & printf '%s\\n' \\\"$!\\\"\"}");
    CHECK(has(&b, "Exit status: 0"));
    pid_t child = (pid_t)strtol(strchr(b.data, '\n') + 1, NULL, 10);
    CHECK(child > 0); CHECK(kill(child, 0) < 0 && errno == ESRCH); buf_free(&b);
    pid_t parent = getpid(), helper = fork(); CHECK(helper >= 0);
    if (!helper) { pause_ms(150); kill(parent, SIGINT); _exit(0); }
    b = tool("sh", "{\"command\":\"sleep 30\"}"); CHECK(has(&b, "Interrupted")); buf_free(&b);
    while (waitpid(helper, NULL, 0) < 0 && errno == EINTR) {}
    interrupted = 0;
    expect_tool("sh", "{\"command\":\"test -z \\\"$OPENAI_API_KEY\\\"\"}", "Exit status: 0");
    expect_tool("sh", "{\"command\":\"grep '^NoNewPrivs:' /proc/self/status\"}", "NoNewPrivs:\t1");
    expect_tool("sh", "{\"command\":\"printf mock-secret\"}", "***********");
    puts("PASS read/write, large files, shell statuses, truncation, timeout, interrupt, descendant reaping and credential handling");
}

static void send_all(int fd, const char *s, size_t n)
{
    while (n) { ssize_t k = write(fd, s, n); if (k < 0 && errno == EINTR) continue;
        CHECK(k > 0); s += k; n -= (size_t)k; }
}

static Buf request(int listener)
{
    static unsigned sequence;
    sequence++;
    struct pollfd p = {.fd = listener, .events = POLLIN};
    CHECK(poll(&p, 1, 10000) == 1);
    char byte; struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } ancillary = {0};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    CHECK(recvmsg(listener, &message, 0) == 1);
    struct cmsghdr *c = CMSG_FIRSTHDR(&message);
    CHECK(c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len == CMSG_LEN(sizeof(int)));
    int fd = *(int *)CMSG_DATA(c); CHECK(fd >= 0);
    Buf b = {0}; size_t header = 0, length = 0;
    for (;;) {
        p.fd = fd; CHECK(poll(&p, 1, 5000) == 1);
        char chunk[8192]; ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) fprintf(stderr, "mock request %u: read %zd, errno %d\n", sequence, n, errno);
        CHECK(n > 0);
        buf_add(&b, chunk, (size_t)n);
        char *end = strstr(b.data, "\r\n\r\n");
        if (end && !header) {
            header = (size_t)(end - b.data) + 4;
            char *cl = strstr(b.data, "Content-Length:"); CHECK(cl && cl < end);
            length = (size_t)strtoull(cl + strlen("Content-Length:"), NULL, 10);
            CHECK(length > 0 && length < 16 * 1024 * 1024);
            CHECK(strstr(b.data, "Authorization: Bearer mock-secret\r\n"));
            const char *url = getenv("SMOLCODE_API_URL");
            CHECK(has(&b, url ? "POST /override/chat/completions HTTP/1.1\r\n" :
                              "POST /v1/chat/completions HTTP/1.1\r\n"));
            CHECK(has(&b, url ? "Host: 127.0.0.1:19084\r\n" : "Host: 127.0.0.1:19083\r\n"));
        }
        if (header && b.len >= header + length) break;
    }
    Buf body = {0}; buf_add(&body, b.data + header, length); buf_free(&b);
    /* Only the test server uses this descriptor, immediately after request(). */
    connection = fd;
    return body;
}

static void respond(int status, const char *body)
{
    Buf reply = {0};
    buf_printf(&reply, "HTTP/1.1 %d Test\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, strlen(body));
    buf_puts(&reply, body); send_all(connection, reply.data, reply.len);
    close(connection); buf_free(&reply);
}

static void final(const char *text)
{
    Buf b = {0}; buf_puts(&b, "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":");
    json_quote(&b, text, strlen(text)); buf_puts(&b, "}}]}"); respond(200, b.data); buf_free(&b);
}

static void call(Buf *b, const char *id, const char *name, const char *args)
{
    buf_puts(b, "{\"id\":"); json_quote(b, id, strlen(id));
    buf_puts(b, ",\"type\":\"function\",\"function\":{\"name\":"); json_quote(b, name, strlen(name));
    buf_puts(b, ",\"arguments\":"); json_quote(b, args, strlen(args)); buf_puts(b, "}}");
}

static void call_response(const char *id, const char *name, const char *args)
{
    Buf b = {0}; buf_puts(&b, "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[");
    call(&b, id, name, args); buf_puts(&b, "]}}]}"); respond(200, b.data); buf_free(&b);
}

static void validate(const Buf *b, Json *j)
{
    const char *model = getenv("SMOLCODE_MODEL");
    CHECK(json_parse(j, b->data, b->len)); CHECK(json_is(j, json_get(j, 0, "model"), model ? model : MODEL));
    size_t t = json_get(j, 0, "tools"); CHECK(t != J_NONE && j->tokens[t].type == J_ARRAY && j->tokens[t].count == 3);
    const char *names[] = {"read", "write", "sh"}; size_t c = t + 1;
    for (size_t i = 0; i < 3; i++) {
        CHECK(json_is(j, json_get(j, json_get(j, c, "function"), "name"), names[i])); c = j->tokens[c].next;
    }
    size_t properties = json_get(j, json_get(j, json_get(j, t + 1, "function"), "parameters"), "properties");
    CHECK(json_is(j, json_get(j, json_get(j, properties, "start_line"), "type"), "integer"));
    CHECK(json_is(j, json_get(j, json_get(j, properties, "end_line"), "type"), "integer"));
    size_t m = json_get(j, 0, "messages"); CHECK(m != J_NONE && j->tokens[m].type == J_ARRAY);
    CHECK(json_is(j, json_get(j, m + 1, "role"), "system"));
    /* Every requested call must have exactly one result before another assistant/user. */
    size_t pending = 0, next_call = J_NONE;
    for (c = m + 1; c < j->tokens[m].next; c = j->tokens[c].next) {
        if (json_is(j, json_get(j, c, "role"), "tool")) {
            CHECK(pending > 0); Buf id = {0};
            CHECK(json_string(j, json_get(j, next_call, "id"), &id));
            CHECK(json_is(j, json_get(j, c, "tool_call_id"), id.data));
            buf_free(&id); pending--; next_call = j->tokens[next_call].next;
        } else {
            CHECK(pending == 0);
            size_t calls = json_get(j, c, "tool_calls");
            if (calls != J_NONE) { pending = j->tokens[calls].count; next_call = calls + 1; }
        }
    }
    CHECK(pending == 0);
}

static size_t last_message(const Json *j)
{
    size_t m = json_get(j, 0, "messages"), last = m + 1;
    for (size_t c = m + 1; c < j->tokens[m].next; c = j->tokens[c].next) last = c;
    return last;
}

static void server(int listener, pid_t cli)
{
    Buf b = request(listener); Json j = {0}; validate(&b, &j);
    CHECK(json_get(&j, 0, "max_completion_tokens") != J_NONE);
    json_free(&j); buf_free(&b);
    respond(400, "{\"error\":{\"param\":\"max_completion_tokens\",\"code\":\"unsupported_parameter\",\"message\":\"unsupported max_completion_tokens\"}}");
    b = request(listener); validate(&b, &j); CHECK(json_get(&j, 0, "max_tokens") != J_NONE);
    json_free(&j); buf_free(&b);
    respond(422, "{\"error\":{\"message\":\"Unknown parameter max_tokens\"}}");
    b = request(listener); validate(&b, &j);
    CHECK(json_get(&j, 0, "max_tokens") == J_NONE && json_get(&j, 0, "max_completion_tokens") == J_NONE);
    CHECK(json_is(&j, json_get(&j, last_message(&j), "content"), "first input \"quotes\" \\ slash"));
    json_free(&j); buf_free(&b);
    Buf r = {0}; buf_puts(&r, "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"  <think>hidden before tools</think>\\nWorking.\\nSecond line.\",\"tool_calls\":[");
    call(&r, "id-\\\"one", "write", "{\"path\":\"integration.txt\",\"content\":\"line1\\nline2 \\\"quote\\\" \\\\ slash \\uD83D\\uDE00\"}");
    buf_puts(&r, ","); call(&r, "id-two", "read", "{\"path\":\"integration.txt\",\"start_line\":2,\"end_line\":2}");
    buf_puts(&r, ","); call(&r, "id-three", "sh", "{\"command\":\"printf shell-ok; printf shell-err >&2\"}");
    buf_puts(&r, "]}}]}"); respond(200, r.data); buf_free(&r);
    b = request(listener); validate(&b, &j);
    CHECK(has(&b, "shell-okshell-err"));
    CHECK(has(&b, "hidden before tools"));
    size_t messages = json_get(&j, 0, "messages"), selected = messages + 1;
    while (!json_is(&j, json_get(&j, selected, "tool_call_id"), "id-two")) selected = j.tokens[selected].next;
    Buf selected_text = {0}; CHECK(json_string(&j, json_get(&j, selected, "content"), &selected_text));
    const char selected_exact[] = "line2 \"quote\" \\ slash \xf0\x9f\x98\x80";
    CHECK(selected_text.len == sizeof(selected_exact) - 1 && !memcmp(selected_text.data, selected_exact, selected_text.len));
    buf_free(&selected_text);
    CHECK(json_is(&j, json_get(&j, last_message(&j), "role"), "tool"));
    json_free(&j); buf_free(&b);
    call_response("id-four", "read", "{\"path\":\"missing-integration\"}");
    b = request(listener); validate(&b, &j); CHECK(has(&b, "Error: open for reading"));
    json_free(&j); buf_free(&b); final("<think>hidden final\nmultiline reasoning</think>\n<think>hidden second block</think>\nFirst final.\nMultiline answer.\n**Strong** and **more**; unmatched ** stays.\n`**code**`\n```c\n**fenced**\n```\n");
    b = request(listener); validate(&b, &j); CHECK(has(&b, "First final.")); CHECK(has(&b, "first input"));
    CHECK(has(&b, "hidden final") && has(&b, "hidden second block"));
    CHECK(has(&b, "**Strong**"));
    json_free(&j); buf_free(&b);
    Buf long_answer = {0}; buf_puts(&long_answer, "Second final.\n");
    for (size_t i = 0; i < 131072; i++) buf_puts(&long_answer, "x");
    final(long_answer.data); buf_free(&long_answer);
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b);
    respond(429, "{\"error\":{\"message\":\"quota mock-secret\"}}");
    b = request(listener); validate(&b, &j); CHECK(has(&b, "rate input")); json_free(&j); buf_free(&b);
    respond(200, "{broken");
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b);
    respond(200, "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"bad\",\"type\":\"function\",\"function\":{\"name\":\"write\",\"arguments\":\"{}\"}},{\"id\":\"bad\",\"type\":\"function\",\"function\":{\"name\":\"sh\",\"arguments\":\"{}\"}}]}}]}");
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b);
    call_response("unknown", "fourth", "{}");
    b = request(listener); validate(&b, &j); CHECK(has(&b, "unknown tool")); json_free(&j); buf_free(&b);
    final("Unknown tool handled.");
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b);
    respond(503, "unavailable");
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b);
    close(connection); /* No response: must report a curl failure. */
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b);
    call_response("bad-args", "read", "{\"path\":42}");
    b = request(listener); validate(&b, &j); CHECK(has(&b, "expected string")); json_free(&j); buf_free(&b);
    final("Invalid arguments handled.");
    for (int i = 0; i < 64; i++) {
        b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b);
        call_response("loop", "read", "{\"path\":\"empty\"}");
    }
    b = request(listener); validate(&b, &j); CHECK(has(&b, "Stopped after 64 tool calls"));
    json_free(&j); buf_free(&b); final("History survived the loop limit.");
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b);
    CHECK(kill(cli, SIGINT) == 0);
    struct pollfd p = {.fd = connection, .events = POLLIN}; CHECK(poll(&p, 1, 5000) == 1);
    char byte; CHECK(read(connection, &byte, 1) == 0); close(connection);
    b = request(listener); validate(&b, &j); CHECK(has(&b, "interrupt api")); json_free(&j); buf_free(&b);
    final("Recovered after API interrupt.");
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b);
    buf_puts(&r, "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[");
    call(&r, "interrupt-sh", "sh", "{\"command\":\"sleep 30\"}");
    buf_puts(&r, ","); call(&r, "skip-write", "write", "{\"path\":\"must-not-exist\",\"content\":\"bad\"}");
    buf_puts(&r, "]}}]}"); respond(200, r.data); buf_free(&r);
    pause_ms(150); CHECK(kill(cli, SIGINT) == 0);
    b = request(listener); validate(&b, &j);
    CHECK(has(&b, "Interrupted.")); CHECK(has(&b, "not executed: operation interrupted"));
    CHECK(access("must-not-exist", F_OK) < 0 && errno == ENOENT);
    json_free(&j); buf_free(&b); final("Recovered after shell interrupt.");
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b);
    respond(200, "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"refusal\":\"Refusal text retained.\"}}]}");
    b = request(listener); validate(&b, &j); CHECK(has(&b, "Refusal text retained."));
    json_free(&j); buf_free(&b); final("Refusal history checked.");
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b); final("<think>hidden unfinished");
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b); final("<think>hidden only</think>");
    b = request(listener); validate(&b, &j); json_free(&j); buf_free(&b);
    final("Literal tags: `<think>example</think>` stay visible.");
    close(listener); _exit(0);
}

static void test_cli(void)
{
    int control[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, control) == 0);
    char number[32]; snprintf(number, sizeof(number), "%d", control[1]);
    CHECK(setenv("SMOLCODE_TEST_FD", number, 1) == 0);
    int input[2], output[2]; CHECK(pipe(input) == 0 && pipe(output) == 0);
    fflush(NULL);
    pid_t cli = fork(); CHECK(cli >= 0);
    if (!cli) {
        close(control[0]);
        close(input[1]); close(output[0]);
        CHECK(dup2(input[0], 0) >= 0 && dup2(output[1], 1) >= 0 && dup2(output[1], 2) >= 0);
        close(input[0]); close(output[1]); execl("./smolcode", "smolcode", (char *)NULL); _exit(127);
    }
    pid_t mock = fork(); CHECK(mock >= 0);
    if (!mock) { close(control[1]); close(input[0]); close(input[1]); close(output[0]); close(output[1]); server(control[0], cli); }
    close(control[0]);
    close(control[1]);
    close(input[0]); close(output[1]);
    const char *lines = "\nfirst input \"quotes\" \\ slash\nsecond input\nrate input\nmalformed input\nduplicate input\nunknown input\nhttp input\nnetwork input\narguments input\nloop input\nafter loop\ninterrupt api\nafter api interrupt\ninterrupt shell\nafter shell interrupt\nrefusal input\nafter refusal\nunfinished reasoning\nreasoning only\nliteral tags\n";
    send_all(input[1], lines, strlen(lines)); close(input[1]);
    Buf transcript = {0};
    for (;;) {
        struct pollfd p = {.fd = output[0], .events = POLLIN}; CHECK(poll(&p, 1, 15000) == 1);
        char chunk[8192]; ssize_t n = read(output[0], chunk, sizeof(chunk)); CHECK(n >= 0);
        if (!n) break;
        buf_add(&transcript, chunk, (size_t)n);
    }
    close(output[0]); int status;
    CHECK(waitpid(cli, &status, 0) == cli && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(waitpid(mock, &status, 0) == mock);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) fwrite(transcript.data, 1, transcript.len, stderr);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(has(&transcript, "smolcode\nmodel: gpt-6")); CHECK(has(&transcript, "> "));
    CHECK(has(&transcript, "Working.\nSecond line.")); CHECK(has(&transcript, "First final.\nMultiline answer."));
    CHECK(has(&transcript, "Second final.")); CHECK(has(&transcript, "HTTP 429 (rate limited)"));
    CHECK(has(&transcript, "malformed API response")); CHECK(has(&transcript, "HTTP 503"));
    CHECK(has(&transcript, "Server returned nothing")); CHECK(!has(&transcript, "mock-secret"));
    CHECK(has(&transcript, "Invalid arguments handled.")); CHECK(has(&transcript, "Stopped after 64 tool calls"));
    CHECK(has(&transcript, "History survived the loop limit."));
    CHECK(has(&transcript, "request interrupted")); CHECK(has(&transcript, "Recovered after API interrupt."));
    CHECK(has(&transcript, "Operation interrupted")); CHECK(has(&transcript, "Recovered after shell interrupt."));
    CHECK(transcript.len > 131072);
    CHECK(has(&transcript, "Refusal text retained.")); CHECK(has(&transcript, "Refusal history checked."));
    CHECK(!has(&transcript, "hidden before tools") && !has(&transcript, "hidden final"));
    CHECK(!has(&transcript, "hidden second block") && !has(&transcript, "hidden unfinished") && !has(&transcript, "hidden only"));
    CHECK(has(&transcript, "model returned reasoning without a visible answer"));
    CHECK(has(&transcript, "Literal tags: `<think>example</think>` stay visible."));
    CHECK(has(&transcript, "Strong and more; unmatched ** stays."));
    CHECK(!has(&transcript, "**Strong**") && !has(&transcript, "\033[1m"));
    CHECK(has(&transcript, "`**code**`") && has(&transcript, "```c\n**fenced**\n```"));
    char *start = strstr(transcript.data, "conversation: "); CHECK(start != NULL); start += strlen("conversation: ");
    char *end = strchr(start, '\n'); CHECK(end != NULL);
    Buf name = {0}; buf_add(&name, start, (size_t)(end - start)); buf_free(&transcript);
    const char *instructions = "New restart instructions.\n";
    file_put("AGENTS.md", instructions, strlen(instructions));
    CHECK(setenv("SMOLCODE_MODEL", "mock:\"small\"\\test", 1) == 0);
    CHECK(setenv("SMOLCODE_API_URL", "http://127.0.0.1:19084/override/chat/completions", 1) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, control) == 0);
    snprintf(number, sizeof(number), "%d", control[1]); CHECK(setenv("SMOLCODE_TEST_FD", number, 1) == 0);
    CHECK(pipe(input) == 0);
    output[0] = posix_openpt(O_RDWR | O_NOCTTY); CHECK(output[0] >= 0);
    CHECK(grantpt(output[0]) == 0 && unlockpt(output[0]) == 0);
    char *terminal = ptsname(output[0]); CHECK(terminal != NULL);
    output[1] = open(terminal, O_WRONLY | O_NOCTTY); CHECK(output[1] >= 0);
    struct termios settings; CHECK(tcgetattr(output[1], &settings) == 0);
    settings.c_oflag &= ~OPOST; CHECK(tcsetattr(output[1], TCSANOW, &settings) == 0);
    CHECK(setenv("TERM", "xterm", 1) == 0); fflush(NULL);
    cli = fork(); CHECK(cli >= 0);
    if (!cli) {
        close(control[0]); close(input[1]); close(output[0]);
        CHECK(dup2(input[0], 0) >= 0 && dup2(output[1], 1) >= 0 && dup2(output[1], 2) >= 0);
        close(input[0]); close(output[1]);
        execl("./smolcode", "smolcode", "--resume", name.data, (char *)NULL); _exit(127);
    }
    close(control[1]); close(input[0]); close(output[1]);
    send_all(input[1], "after restart\n", strlen("after restart\n")); close(input[1]);
    Buf b = request(control[0]); Json j = {0}; validate(&b, &j);
    CHECK(has(&b, "New restart instructions")); CHECK(has(&b, "First final")); CHECK(has(&b, "id-three"));
    CHECK(has(&b, "Refusal history checked")); CHECK(has(&b, "Second final"));
    CHECK(json_is(&j, json_get(&j, last_message(&j), "content"), "after restart"));
    json_free(&j); buf_free(&b); final("Resumed conversation checked. **Bold text** and **second**; unmatched ** stays.\n`**code**`\n```\n**fenced**\n```\n"); close(control[0]);
    for (;;) {
        struct pollfd p = {.fd = output[0], .events = POLLIN}; CHECK(poll(&p, 1, 10000) == 1);
        char chunk[8192]; ssize_t n = read(output[0], chunk, sizeof(chunk));
        if (n < 0 && errno == EIO) break; /* A closed PTY slave signals EOF with EIO. */
        CHECK(n >= 0);
        if (!n) break;
        buf_add(&transcript, chunk, (size_t)n);
    }
    close(output[0]); CHECK(waitpid(cli, &status, 0) == cli && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(has(&transcript, "Resumed conversation checked."));
    CHECK(has(&transcript, "\033[1mBold text\033[22m and \033[1msecond\033[22m; unmatched ** stays."));
    CHECK(has(&transcript, "`**code**`") && has(&transcript, "```\n**fenced**\n```"));
    CHECK(has(&transcript, "model: mock:\"small\"\\test\n"));
    Buf path = {0}, saved = {0}; buf_printf(&path, ".smolcode/%s.txt", name.data);
    CHECK(text_load(path.data, &saved) == 1); CHECK(has(&saved, "after restart")); CHECK(has(&saved, "Resumed conversation checked."));
    CHECK(has(&saved, "**Bold text**") && !has(&saved, "\033[1m"));
    buf_free(&path); buf_free(&saved); buf_free(&name); buf_free(&transcript); CHECK(unlink("AGENTS.md") == 0);
    CHECK(unsetenv("SMOLCODE_MODEL") == 0 && unsetenv("SMOLCODE_API_URL") == 0);
    puts("PASS mocked Chat Completions: three tools, multi-call/multi-step loop, roles/IDs, history, multiline, parameter fallback, API errors and loop limit");
}

static void test_startup(void)
{
    for (int mode = 0; mode < 7; mode++) {
        int input[2], output[2]; CHECK(pipe(input) == 0 && pipe(output) == 0); fflush(NULL);
        pid_t cli = fork(); CHECK(cli >= 0);
        if (!cli) {
            close(input[1]); close(output[0]);
            CHECK(dup2(input[0], 0) >= 0 && dup2(output[1], 1) >= 0 && dup2(output[1], 2) >= 0);
            close(input[0]); close(output[1]);
            if (mode == 0) unsetenv("OPENAI_API_KEY");
            if (mode == 1) setenv("OPENAI_API_KEY", "bad\nkey", 1);
            if (mode == 4) setenv("SMOLCODE_MODEL", "", 1);
            if (mode == 5) setenv("SMOLCODE_API_URL", "", 1);
            if (mode == 6) setenv("SMOLCODE_API_URL", "file:///tmp/not-an-api", 1);
            if (mode == 2) execl("./smolcode", "smolcode", "--model", (char *)NULL);
            else execl("./smolcode", "smolcode", (char *)NULL);
            _exit(127);
        }
        close(input[0]); close(output[1]);
        if (mode == 3) { pause_ms(150); CHECK(kill(cli, SIGINT) == 0); pause_ms(50); }
        close(input[1]); Buf transcript = {0};
        for (;;) {
            char chunk[1024]; ssize_t n = read(output[0], chunk, sizeof(chunk)); CHECK(n >= 0);
            if (!n) break;
            buf_add(&transcript, chunk, (size_t)n);
        }
        close(output[0]); int status;
        CHECK(waitpid(cli, &status, 0) == cli && WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == (mode == 3 ? 0 : 1));
        if (mode < 2) CHECK(has(&transcript, "set OPENAI_API_KEY"));
        if (mode == 2) CHECK(has(&transcript, "Usage: smolcode"));
        if (mode == 3) CHECK(has(&transcript, "> \n> "));
        if (mode == 4) CHECK(has(&transcript, "SMOLCODE_MODEL"));
        if (mode >= 5) CHECK(has(&transcript, "SMOLCODE_API_URL"));
        buf_free(&transcript);
    }
    puts("PASS missing/invalid credential, empty/invalid environment settings, no CLI settings, Ctrl+C at prompt, Ctrl+D/EOF");
}

static void discard(Agent *a) { buf_free(&a->messages); buf_free(&a->session); }

static void file_put(const char *path, const char *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600); CHECK(fd >= 0);
    send_all(fd, data, len); CHECK(close(fd) == 0);
}

static void test_sessions(void)
{
    const char *before = "Before-resume project instructions.\n";
    file_put("AGENTS.md", before, strlen(before));
    Agent a, other, resumed;
    CHECK(agent_init(&a)); CHECK(has(&a.messages, "Before-resume project instructions."));
    CHECK(session_start(&a, NULL)); CHECK(agent_init(&other)); CHECK(session_start(&other, NULL));
    CHECK(strcmp(a.session.data, other.session.data) != 0); discard(&other);
    struct stat st; CHECK(stat(a.session.data, &st) == 0 && (st.st_mode & 0777) == 0600);
    Buf name = {0}; buf_add(&name, a.session.data + 10, a.session.len - 14);
    buf_puts(&a.messages, ",{\"role\":\"user\",\"content\":\"first\\nassistant 0 0\\nquotes \\\" \\\\ \\uD83D\\uDE00 \\u0000 end\"},"
        "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
        "{\"id\":\"read-one\",\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"file.c\\\"}\"}},"
        "{\"id\":\"sh-two\",\"type\":\"function\",\"function\":{\"name\":\"sh\",\"arguments\":\"{\\\"command\\\":\\\"touch must-not-replay\\\"}\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"read-one\",\"content\":\"saved result mock-secret\\nline2\"}");
    CHECK(session_save(&a)); Buf plain = {0}; CHECK(text_load(a.session.data, &plain) == 1);
    CHECK(has(&plain, "Smolcode conversation 1\n\nuser "));
    CHECK(has(&plain, "first\nassistant 0 0\nquotes")); CHECK(has(&plain, "\\x00"));
    CHECK(!memchr(plain.data, 0, plain.len)); CHECK(!has(&plain, "mock-secret"));
    CHECK(!has(&plain, "Before-resume project instructions")); buf_free(&plain);
    const char *after = "After-resume project instructions.\n";
    file_put("AGENTS.md", after, strlen(after));
    CHECK(agent_init(&resumed)); CHECK(session_start(&resumed, name.data));
    CHECK(has(&resumed.messages, "After-resume project instructions")); CHECK(!has(&resumed.messages, "Before-resume project instructions"));
    CHECK(has(&resumed.messages, "execution status unknown")); CHECK(has(&resumed.messages, "saved result ***********"));
    CHECK(access("must-not-replay", F_OK) < 0 && errno == ENOENT);
    Buf wrapped = {0}; Json j = {0}; buf_puts(&wrapped, "["); buf_add(&wrapped, resumed.messages.data, resumed.messages.len); buf_puts(&wrapped, "]");
    CHECK(json_parse(&j, wrapped.data, wrapped.len)); size_t count = j.tokens[0].count;
    size_t user = 1;
    while (!json_is(&j, json_get(&j, user, "role"), "user")) user = j.tokens[user].next;
    Buf restored = {0}; CHECK(json_string(&j, json_get(&j, user, "content"), &restored));
    const char exact[] = "first\nassistant 0 0\nquotes \" \\ \xf0\x9f\x98\x80 \0 end";
    CHECK(restored.len == sizeof(exact) - 1 && !memcmp(restored.data, exact, restored.len));
    buf_free(&restored); json_free(&j); buf_free(&wrapped); discard(&resumed);
    CHECK(agent_init(&resumed)); CHECK(session_start(&resumed, name.data));
    buf_puts(&wrapped, "["); buf_add(&wrapped, resumed.messages.data, resumed.messages.len); buf_puts(&wrapped, "]");
    CHECK(json_parse(&j, wrapped.data, wrapped.len)); CHECK(j.tokens[0].count == count);
    json_free(&j); buf_free(&wrapped); discard(&resumed);
    CHECK(agent_init(&resumed)); size_t base = resumed.messages.len;
    CHECK(!session_start(&resumed, "../outside")); CHECK(resumed.messages.len == base); discard(&resumed);
    CHECK(agent_init(&resumed)); CHECK(!session_start(&resumed, "missing-missing-missing")); discard(&resumed);
    const char *bad = "Smolcode conversation 1\n\nuser 184467440737095516160\nx\n";
    file_put(".smolcode/bad-bad-bad.txt", bad, strlen(bad));
    CHECK(agent_init(&resumed)); base = resumed.messages.len;
    CHECK(!session_start(&resumed, "bad-bad-bad")); CHECK(resumed.messages.len == base); discard(&resumed);
    CHECK(text_load(".smolcode/bad-bad-bad.txt", &plain) == 1); CHECK(plain.len == strlen(bad) && !memcmp(plain.data, bad, plain.len)); buf_free(&plain);
    bad = "Smolcode conversation 1\n\nuser 20\nshort\n";
    file_put(".smolcode/bad-bad-bad.txt", bad, strlen(bad));
    CHECK(agent_init(&resumed)); CHECK(!session_start(&resumed, "bad-bad-bad")); discard(&resumed);
    CHECK(agent_init(&resumed)); buf_puts(&resumed.session, ".smolcode/absent-dir/file.txt");
    CHECK(!session_save(&resumed) && resumed.save_failed); discard(&resumed);
    CHECK(unlink("AGENTS.md") == 0); buf_free(&name); discard(&a);
    puts("PASS TXT conversations, random names, exact resume, fresh AGENTS.md, interrupted calls, secret masking and malformed/save failures");
}

int main(void)
{
    CHECK(unsetenv("SMOLCODE_MODEL") == 0 && unsetenv("SMOLCODE_API_URL") == 0);
    CHECK(setenv("OPENAI_API_KEY", "mock-secret", 1) == 0);
    CHECK(setenv("NO_PROXY", "127.0.0.1", 1) == 0);
    struct sigaction sa = {.sa_handler = handler}; sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGINT, &sa, NULL) == 0); signal(SIGPIPE, SIG_IGN);
    test_json(); test_tools(); test_cli(); test_startup(); test_sessions();
    printf("PASS %u parent-process assertions (plus mock-server protocol assertions)\n", checks);
    return 0;
}
