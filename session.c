#define _POSIX_C_SOURCE 200809L
#include "smolcode.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

static const char signature[] = "Smolcode conversation 1\n\n";

/* A missing optional file returns zero; other failures are reported. */
int text_load(const char *path, Buf *out)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { if (errno == ENOENT) return 0; perror(path); return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "smolcode: %s must be a readable regular file\n", path); close(fd); return -1;
    }
    for (;;) {
        char chunk[8192]; ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR && !interrupted) continue;
        if (n < 0 || interrupted) { if (interrupted) errno = EINTR; perror(path); close(fd); return -1; }
        if (!n) break;
        buf_add(out, chunk, (size_t)n);
    }
    close(fd); buf_add(out, "", 0); return 1;
}

/* Plain text fields retain real newlines. Escape backslashes and nontext bytes. */
static Buf field(const Json *j, size_t token)
{
    Buf raw = {0}, text = {0}; json_string(j, token, &raw); redact(&raw);
    for (size_t i = 0; i < raw.len; i++) {
        unsigned char c = (unsigned char)raw.data[i];
        if (c == '\\') buf_puts(&text, "\\\\");
        else if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f) buf_printf(&text, "\\x%02x", c);
        else buf_add(&text, &c, 1);
    }
    buf_free(&raw); return text;
}

static void put_field(Buf *file, const Buf *text)
{
    buf_add(file, text->data, text->len); buf_puts(file, "\n");
}

int session_save(Agent *a)
{
    if (!a->session.len) return 1;
    Buf wrapped = {0}, file = {0}, temporary = {0}; Json j = {0};
    buf_puts(&wrapped, "["); buf_add(&wrapped, a->messages.data, a->messages.len); buf_puts(&wrapped, "]");
    if (!json_parse(&j, wrapped.data, wrapped.len)) {
        fputs("smolcode: cannot save invalid conversation history\n", stderr); a->save_failed = 1; buf_free(&wrapped); return 0;
    }
    buf_puts(&file, signature);
    for (size_t m = 1; m < j.tokens[0].next; m = j.tokens[m].next) {
        size_t role = json_get(&j, m, "role");
        if (json_is(&j, role, "system")) continue; /* Reload current AGENTS.md on resume. */
        Buf content = field(&j, json_get(&j, m, "content"));
        if (json_is(&j, role, "tool")) {
            Buf id = field(&j, json_get(&j, m, "tool_call_id"));
            buf_printf(&file, "tool %zu %zu\n", id.len, content.len);
            put_field(&file, &id); put_field(&file, &content); buf_free(&id);
        } else if (json_is(&j, role, "assistant")) {
            size_t calls = json_get(&j, m, "tool_calls");
            size_t count = calls == J_NONE ? 0 : j.tokens[calls].count;
            buf_printf(&file, "assistant %zu %zu\n", content.len, count); put_field(&file, &content);
            for (size_t c = calls == J_NONE ? j.count : calls + 1;
                 count && c < j.tokens[calls].next; c = j.tokens[c].next) {
                size_t f = json_get(&j, c, "function");
                Buf id = field(&j, json_get(&j, c, "id")), name = field(&j, json_get(&j, f, "name"));
                Buf args = field(&j, json_get(&j, f, "arguments"));
                buf_printf(&file, "call %zu %zu %zu\n", id.len, name.len, args.len);
                put_field(&file, &id); put_field(&file, &name); put_field(&file, &args);
                buf_free(&id); buf_free(&name); buf_free(&args);
            }
        } else { buf_printf(&file, "user %zu\n", content.len); put_field(&file, &content); }
        buf_free(&content);
    }
    json_free(&j); buf_free(&wrapped);
    buf_printf(&temporary, "%s.tmp.XXXXXX", a->session.data);
    int fd = mkstemp(temporary.data), ok = fd >= 0;
    if (ok) {
        size_t done = 0;
        while (done < file.len) {
            size_t size = file.len - done; if (size > 65536) size = 65536;
            ssize_t n = write(fd, file.data + done, size);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { if (!n) errno = EIO; ok = 0; break; }
            done += (size_t)n;
        }
        if (ok) {
            int rc; do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);
            if (rc < 0) ok = 0;
        }
        int saved_errno = errno;
        if (close(fd) < 0 && ok) { saved_errno = errno; ok = 0; }
        errno = saved_errno;
        if (ok && rename(temporary.data, a->session.data) < 0) ok = 0;
    }
    if (!ok) {
        perror("smolcode: save conversation"); unlink(temporary.data); a->save_failed = 1;
    }
    buf_free(&file); buf_free(&temporary); return ok;
}

/* Length framing makes newlines and apparent role headers inside text unambiguous. */
static int header(const Buf *file, size_t *pos, const char *role, size_t *sizes, size_t count)
{
    size_t p = *pos, n = strlen(role);
    if (p > file->len || n >= file->len - p || memcmp(file->data + p, role, n) || file->data[p + n] != ' ') return 0;
    p += n + 1;
    for (size_t i = 0; i < count; i++) {
        size_t value = 0, start = p;
        while (p < file->len && file->data[p] >= '0' && file->data[p] <= '9') {
            unsigned digit = (unsigned)(file->data[p++] - '0');
            if (value > (SIZE_MAX - digit) / 10) return 0;
            value = value * 10 + digit;
        }
        if (p == start || p == file->len || file->data[p++] != (i + 1 == count ? '\n' : ' ')) return 0;
        sizes[i] = value;
    }
    *pos = p; return 1;
}

static int hex(unsigned char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int take(const Buf *file, size_t *pos, size_t len, Buf *out)
{
    if (*pos >= file->len || len > file->len - *pos - 1 || file->data[*pos + len] != '\n') return 0;
    const char *s = file->data + *pos;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            if (++i == len) return 0;
            if (s[i] == '\\') c = '\\';
            else if (s[i] == 'x' && len - i >= 3 && hex((unsigned char)s[i + 1]) >= 0 && hex((unsigned char)s[i + 2]) >= 0) {
                c = (unsigned char)(hex((unsigned char)s[i + 1]) * 16 + hex((unsigned char)s[i + 2])); i += 2;
            } else return 0;
        } else if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f) return 0;
        buf_add(out, &c, 1);
    }
    buf_add(out, "", 0); *pos += len + 1; return 1;
}

static int load(Agent *a)
{
    Buf file = {0}; size_t original = a->messages.len;
    int found = text_load(a->session.data, &file), ok = found == 1;
    if (!found) fputs("smolcode: conversation not found\n", stderr);
    size_t pos = sizeof(signature) - 1;
    if (ok && (file.len < pos || memcmp(file.data, signature, pos))) ok = 0;
    while (ok && pos < file.len) {
        size_t sizes[3]; Buf content = {0}, id = {0};
        if (header(&file, &pos, "user", sizes, 1)) {
            ok = take(&file, &pos, sizes[0], &content);
            buf_puts(&a->messages, ",{\"role\":\"user\",\"content\":");
            json_quote(&a->messages, content.data, content.len); buf_puts(&a->messages, "}");
        } else if (header(&file, &pos, "tool", sizes, 2)) {
            ok = take(&file, &pos, sizes[0], &id) && take(&file, &pos, sizes[1], &content);
            buf_puts(&a->messages, ",{\"role\":\"tool\",\"tool_call_id\":"); json_quote(&a->messages, id.data, id.len);
            buf_puts(&a->messages, ",\"content\":"); json_quote(&a->messages, content.data, content.len); buf_puts(&a->messages, "}");
        } else if (header(&file, &pos, "assistant", sizes, 2)) {
            size_t calls = sizes[1];
            ok = take(&file, &pos, sizes[0], &content) && calls <= file.len - pos;
            buf_puts(&a->messages, ",{\"role\":\"assistant\",\"content\":"); json_quote(&a->messages, content.data, content.len);
            if (calls) buf_puts(&a->messages, ",\"tool_calls\":[");
            for (size_t i = 0; ok && i < calls; i++) {
                Buf name = {0}, args = {0};
                ok = header(&file, &pos, "call", sizes, 3) && take(&file, &pos, sizes[0], &id) &&
                     take(&file, &pos, sizes[1], &name) && take(&file, &pos, sizes[2], &args);
                if (i) buf_puts(&a->messages, ",");
                buf_puts(&a->messages, "{\"id\":"); json_quote(&a->messages, id.data, id.len);
                buf_puts(&a->messages, ",\"type\":\"function\",\"function\":{\"name\":"); json_quote(&a->messages, name.data, name.len);
                buf_puts(&a->messages, ",\"arguments\":"); json_quote(&a->messages, args.data, args.len); buf_puts(&a->messages, "}}");
                buf_free(&id); buf_free(&name); buf_free(&args);
            }
            if (calls) buf_puts(&a->messages, "]");
            buf_puts(&a->messages, "}");
        } else ok = 0;
        buf_free(&content); buf_free(&id);
    }
    if (ok) ok = agent_recover(a);
    if (!ok) {
        if (found == 1) fputs("smolcode: malformed conversation; file left unchanged\n", stderr);
        a->messages.len = original; a->messages.data[original] = '\0';
    }
    buf_free(&file); return ok;
}

int session_start(Agent *a, const char *name)
{
    if (name) {
        unsigned words = 0; size_t len = strlen(name);
        if (!len || len > 128) { fputs("smolcode: invalid conversation name\n", stderr); return 0; }
        for (size_t i = 0; i < len; i++) {
            if (name[i] == '-' && i && i + 1 < len && name[i - 1] != '-') words++;
            else if (name[i] < 'a' || name[i] > 'z') words = 100;
        }
        if (words != 2) { fputs("smolcode: resume name must be three lowercase words separated by hyphens\n", stderr); return 0; }
        struct stat st;
        if (lstat(".smolcode", &st) == 0 && !S_ISDIR(st.st_mode)) {
            fputs("smolcode: .smolcode must be a directory\n", stderr); return 0;
        }
        buf_printf(&a->session, ".smolcode/%s.txt", name);
        return load(a) && session_save(a);
    }
    if (mkdir(".smolcode", 0700) < 0 && errno != EEXIST) { perror(".smolcode"); return 0; }
    struct stat st;
    if (lstat(".smolcode", &st) < 0 || !S_ISDIR(st.st_mode)) { fputs("smolcode: .smolcode must be a directory\n", stderr); return 0; }
    static const char *words[] = {
        "amber", "birch", "cedar", "dawn", "ember", "fern", "glade", "harbor",
        "iris", "jade", "kite", "lake", "moss", "north", "oak", "pine",
        "quiet", "river", "stone", "tide", "umber", "vale", "willow", "zenith",
        "acorn", "breeze", "cloud", "dune", "echo", "frost", "grove", "hill",
        "island", "juniper", "kelp", "leaf", "maple", "night", "orbit", "pebble",
        "quartz", "rain", "sage", "thistle", "urchin", "violet", "wave", "yarrow",
        "ash", "brook", "clover", "drift", "elm", "finch", "grass", "haze",
        "indigo", "jewel", "kiwi", "lily", "moon", "nest", "otter", "petal"
    };
    for (int attempt = 0; attempt < 128; attempt++) {
        unsigned char random[3]; size_t got = 0;
        while (got < sizeof(random)) {
            ssize_t n = getrandom(random + got, sizeof(random) - got, 0);
            if (n < 0 && errno == EINTR && !interrupted) continue;
            if (n <= 0 || interrupted) { perror("smolcode: random name"); return 0; }
            got += (size_t)n;
        }
        buf_free(&a->session);
        buf_printf(&a->session, ".smolcode/%s-%s-%s.txt", words[random[0] % 64], words[random[1] % 64], words[random[2] % 64]);
        int fd = open(a->session.data, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            close(fd);
            if (session_save(a)) return 1;
            unlink(a->session.data); return 0;
        }
        if (errno != EEXIST) { perror("smolcode: create conversation"); return 0; }
    }
    fputs("smolcode: could not find an unused conversation name\n", stderr); return 0;
}
