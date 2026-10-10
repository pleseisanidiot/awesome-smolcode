# Smolcode

A small C11 terminal coding agent. Exactly three tools: `read`, `write`, `sh`.
Conversation files are plain text. Features are direct C source changes.

## Build and run

Requires Linux, GCC or Clang, Make, and libcurl development headers/library.
The JSON parser is embedded C; there are no other dependencies.

```sh
make
export OPENAI_API_KEY="ollama"
./smolcode
```

Run from the directory you want to work in. Enter one request per input line;
assistant responses can contain any number of lines. Empty input is ignored.
Leading `<think>...</think>` reasoning blocks are hidden in terminal responses;
the original responses remain in conversation history. An unfinished leading
reasoning block is hidden too; if no answer follows, a notice is printed.
Literal tags elsewhere in an answer remain visible.
Paired `**text**` within a line is rendered bold on terminals and as plain text when redirected
or when `TERM=dumb`. Unmatched markers and backtick code are left literal.
Saved conversations and model history retain the original Markdown.
Ctrl+D exits. Ctrl+C cancels a request or tool operation and returns to the
prompt. Conversation history is saved in the current project's `.smolcode/`,
including tool results and prior requests that failed. No history is silently
trimmed; if the provider's context limit is reached, its error is reported.

Set `SMOLCODE_MODEL` and `SMOLCODE_API_URL` to choose a model and endpoint without
recompiling. Each unset variable falls back to its default in `config.h`;
explicitly empty values produce a startup error. The endpoint must use `http://`
or `https://`. Settings are read once at startup, including when resuming.

```sh
export SMOLCODE_API_URL="http://localhost:11434/v1/chat/completions"
export SMOLCODE_MODEL="your-installed-model" # Choose a name from ollama list.
export OPENAI_API_KEY="ollama"
./smolcode
```

Edit `config.h` and run `make` to change the fallback model/endpoint, credential
environment variable, token limit, tool output limit, or command timeout. The
model is an arbitrary string. The default is `qwen3.5:9b` on local Ollama at
`http://127.0.0.1:11434/v1/chat/completions`; that model must be installed in the
running Ollama instance. Smolcode requires a nonempty
`OPENAI_API_KEY`; use the placeholder `ollama` for this local endpoint. For a
remote authenticated provider, set its real key instead.
The only command-line option is `--resume`.

```sh
make clean
make CC=clang
# Optional, performed by the human:
sudo make install
```

Installation defaults to `/usr/local/bin`; `PREFIX` and `DESTDIR` are supported.

## Conversations and instructions

`smolcode` starts a new conversation and prints its random three-word name, such
as `amber-quiet-river`. It saves to `.smolcode/amber-quiet-river.txt` in the current
working directory. To resume, run from that same project directory:

```sh
smolcode --resume amber-quiet-river
```

Use `ls .smolcode/*.txt` to find conversations and any text editor to read them.
Each file contains role-labeled messages and native tool-call metadata. Message
contents retain real line breaks, quotes, and Unicode; backslashes are doubled
and nontext control bytes use `\xHH`. Small headers hold byte counts so text that
resembles a role header cannot corrupt the restored history. These are readable
text files, not a database or a JSON conversation dump. Manual edits must also
update the byte counts. Malformed or missing conversations produce a clear error;
malformed files are left unchanged.

Conversation files are created with permissions `0600`, and new `.smolcode/`
directories with `0700`. Each checkpoint uses a temporary file and atomic
replacement. Smolcode saves the user message before sending it, the assistant's
requested calls before executing them, and every result afterward. If saving
fails, it stops rather than silently continuing without persistence. Use one
process per conversation; simultaneous writers are not coordinated.

If a process ends with a pending call, resume marks that call's execution status
as unknown. It never automatically replays the command or claims it succeeded:
the model must inspect current state before retrying. Completed tool results and
call IDs are restored. The configured API key is masked in saved fields.

Put project instructions and reusable guidance in the current directory's
`AGENTS.md`. Smolcode reads that file at startup, including when resuming, and adds
it to the model's system instructions. Missing `AGENTS.md` is fine; unreadable or
nonregular files produce an error. System instructions are freshly loaded rather
than copied from an old conversation. There is no skill loader or add-on system;
new behavior means patching the C source and recompiling. Keep the project under
5,000 lines of code.

## Tools

| Tool | Arguments | Result |
| --- | --- | --- |
| `read` | `{"path":"main.c","start_line":10,"end_line":40}` | Selected lines of a regular file, or a clear error. |
| `write` | `{"path":"main.c","content":"..."}` | Creates or overwrites that file, reports bytes written or an error. |
| `sh` | `{"command":"make"}` | Combined stdout/stderr, exit status or signal, timeout/interruption information. |

Arguments must be JSON objects with the stated fields and no extra fields.
`read` requires a string `path`; its optional `start_line` and `end_line` must be
positive integers, with `end_line >= start_line`. Lines are one-based and both
bounds are inclusive. Omitted bounds mean line 1 and EOF; omitting both reads the
whole file. A range ending beyond EOF returns the remaining contents. A starting
line beyond EOF (including a range in an empty file) returns an explicit error.
Selected bytes preserve line breaks, including CRLF and a final unterminated line.
Paths and commands cannot be empty or contain NUL. File contents can contain NUL,
Unicode, quotes, percent signs, and line breaks; writes preserve their decoded
bytes. Parent directories must already exist. File tools reject nonregular files.
Writes are direct, not atomic: interruption or disk errors can leave a partial
file, which is reported.

`read` refuses selections larger than `MAX_TOOL_OUTPUT` (default 65,536 bytes),
returning an explicit error with no partial contents. Read large files in chunks
using `start_line`/`end_line`; the reader scans with bounded memory rather than
loading the whole file. A single line exceeding the limit requires `sh` with
`head`, `cut`, or another command to inspect portions. Shell capture keeps the first `MAX_TOOL_OUTPUT`
bytes, drains the remaining output, and appends an explicit truncation notice.
UTF-8 is preserved. Non-UTF-8 output bytes are represented as Unicode U+00XX in
the JSON sent to the model; use `sh` with `od`/`xxd` for an unambiguous binary dump.

Commands run through `/bin/sh -c`, with stdin connected to `/dev/null`. Commands
that require interactive input are unsuitable. Each command gets a separate
process group. Timeout (default 60 seconds), Ctrl+C, and normal shell exit clean
up remaining jobs in that group; Linux child reaping prevents ordinary background
jobs from becoming zombies. Commands that deliberately detach into another
process group can escape this cleanup. File tools check for cancellation between
I/O operations; an OS-level blocking disk operation may delay cancellation.

## API behavior

Uses non-streaming OpenAI-compatible Chat Completions with native function tools.
Multiple requested calls execute in order, and their results retain the original
call IDs and `tool` roles. Tool contents are JSON-escaped data, never inserted into
the system prompt or treated as executable instructions by the C loop. Unknown
tools and invalid arguments produce error results, not fabricated successes.
Malformed message/call structure is rejected before executing the batch.

Requests start with `max_completion_tokens`. If the endpoint explicitly rejects
that parameter, Smolcode retries with `max_tokens`. If that is also rejected, it
retries without a token parameter, prints a notice, and uses the provider's
default output limit. The successful setting is remembered in memory. Other
API/HTTP errors, rate limits, and transport failures are reported without
automatic retry; enter a new request to continue. Connection timeout is 15
seconds and total request timeout is 300 seconds. For HTTPS endpoints, certificate
verification is enabled by libcurl; redirects are not followed.

A user turn stops after 64 tool calls. Completed results and explicit results for
unexecuted calls remain in history, so the next request can continue coherently.

The protocol follows the [official Chat Completions reference](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create).

## Security

**This is not a sandbox.** Model commands execute with the current user's
permissions and can read, modify, or delete accessible files and access the
network. File contents and command output can be sent to the configured API.
Only use this agent where you accept that access; do not run it as root.

Smolcode does not invoke privilege elevation. Shell children use Linux
`no_new_privs`, preventing privilege gains through setuid executables or file
capabilities. This does not restrict normal user permissions or provide a
filesystem/network sandbox. The configured API key comes only from the
environment, is removed from the shell child's environment, and is masked if
it appears in displayed text or returned tool output. Commands can still inspect
other accessible files/processes; masking is not a secret isolation boundary.
Conversation text and tool results are saved in `.smolcode/`; they may contain
sensitive project information. The configured API key is masked before saving.

## Verification

```sh
make check
# Optional sanitizer run (Clang):
CC=clang TEST_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  ASAN_OPTIONS='detect_leaks=1:halt_on_error=1' \
  UBSAN_OPTIONS='halt_on_error=1' sh tests/run.sh
```

Tests use C and POSIX shell, a dummy key, a one-second command timeout, and a
disposable source/build directory. Production `config.h` and the executable are
unchanged. The C mock server exchanges actual HTTP requests/responses with
libcurl over local Unix socket pairs. A test-only curl wrapper supplies connected
descriptors; a test-only I/O shim uses local IPC `read`/`write` because the build
session blocks TCP, listener binding, and socket `send`/`recv`. Neither wrapper
is linked into the production executable.

Coverage includes terminal input/EOF, prompt and operation interruption,
existing/missing/oversized files, line ranges across buffer boundaries, omitted
bounds, EOF/CRLF/NUL handling, invalid bounds, exact creation/overwrite, a 1 MiB write,
successful/failing/missing shell commands, combined output, output truncation,
timeout, background child cleanup, JSON escapes/Unicode/NUL, malformed JSON,
multiline and large assistant responses, multiple conversation turns, several
tools per turn, successive tool rounds, call IDs/roles, rejected parameters,
HTTP/rate-limit/transport failures, invalid/unknown tools, and the loop limit.
Tests also restart the executable with `--resume`, verify prior calls/results and
new `AGENTS.md` instructions reach the API, verify default and environment model/
endpoint settings (including escaped model names and invalid values), round-trip multiline/Unicode/NUL
contents, check random names and file permissions, repair interrupted histories
without replay, and reject malformed files and report save failures.
Mock responses also verify reasoning blocks are hidden before tool calls and
final answers, retained in history, and handled when repeated or unfinished.
Bold output is checked through a pseudo-terminal and redirected pipes, including
unmatched markers, literal code, and unchanged Markdown in saved conversations.

These are mocked integration tests. No live model, real credential, external
provider connection, or TLS exchange was used in verification.

Both GCC and Clang builds passed with warnings treated as errors. The suite also
passed AddressSanitizer and UndefinedBehaviorSanitizer with leak detection
disabled; this host blocks LeakSanitizer's required ptrace support. Clang's static
analyzer reported no findings.
