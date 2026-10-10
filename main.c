#define _POSIX_C_SOURCE 200809L
#include "smolcode.h"
#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

volatile sig_atomic_t interrupted;

static void interrupt(int sig) { (void)sig; interrupted = 1; }

int main(int argc, char **argv)
{
    const char *resume = NULL;
    if (argc == 3 && !strcmp(argv[1], "--resume")) resume = argv[2];
    else if (argc != 1) { fputs("Usage: smolcode [--resume word-word-word]\n", stderr); return 1; }
    const char *key = getenv(API_KEY_ENV);
    if (!key || !*key || strchr(key, '\r') || strchr(key, '\n')) {
        fprintf(stderr, "smolcode: set %s to a nonempty API key without newlines\n", API_KEY_ENV); return 1;
    }
    if (MAX_OUTPUT_TOKENS <= 0 || MAX_TOOL_OUTPUT <= 0 || TOOL_TIMEOUT_SECONDS <= 0) {
        fputs("smolcode: config.h limits must be positive\n", stderr); return 1;
    }
    struct sigaction sa = {.sa_handler = interrupt};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0) { perror("sigaction"); return 1; }
    signal(SIGPIPE, SIG_IGN);
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) { fprintf(stderr, "smolcode: %s\n", curl_easy_strerror(rc)); return 1; }
    Agent a;
    if (!agent_init(&a) || !session_start(&a, resume)) {
        buf_free(&a.messages); buf_free(&a.session); curl_global_cleanup(); return 1;
    }
    fputs("smolcode\nmodel: ", stdout); safe_print(a.model, strlen(a.model));
    fputs("\nconversation: ", stdout);
    safe_print(a.session.data + 10, a.session.len - 14); putchar('\n'); putchar('\n');
    int result = 0;
    char *line = NULL; size_t cap = 0;
    for (;;) {
        interrupted = 0;
        fputs("> ", stdout); fflush(stdout);
        errno = 0; ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            if (interrupted || errno == EINTR) { clearerr(stdin); putchar('\n'); continue; }
            if (ferror(stdin)) perror("smolcode: input");
            break;
        }
        if (n && line[n - 1] == '\n') n--;
        if (!n) continue;
        if (interrupted) { putchar('\n'); continue; }
        agent_turn(&a, line, (size_t)n);
        if (a.save_failed) { result = 1; break; }
        putchar('\n');
        /* Reap already-exited detached descendants, without blocking input. */
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
    }
    putchar('\n'); free(line); buf_free(&a.messages); buf_free(&a.session); curl_global_cleanup();
    return result;
}
