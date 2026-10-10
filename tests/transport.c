/* Test-only transport: real libcurl HTTP over a local Unix socket.
 * Linked only into the disposable test CLI, never into production. */
#include <curl/curl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static curl_socket_t open_local(void *data, curlsocktype purpose, struct curl_sockaddr *address)
{
    (void)data; (void)purpose; (void)address;
    const char *control = getenv("SMOLCODE_TEST_FD");
    int pair[2];
    if (!control || socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) return CURL_SOCKET_BAD;
    char byte = 'H'; struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } ancillary = {0};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&message);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    *(int *)CMSG_DATA(c) = pair[1];
    if (sendmsg(atoi(control), &message, 0) != 1) { close(pair[0]); close(pair[1]); return CURL_SOCKET_BAD; }
    close(pair[1]); return pair[0];
}

static int connected(void *data, curl_socket_t socket, curlsocktype purpose)
{
    (void)data; (void)socket; (void)purpose;
    return CURL_SOCKOPT_ALREADY_CONNECTED;
}

CURLcode __real_curl_easy_perform(CURL *curl);
CURLcode __wrap_curl_easy_perform(CURL *curl)
{
    CURLcode rc = curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, open_local);
    if (rc == CURLE_OK) rc = curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, connected);
    return rc == CURLE_OK ? __real_curl_easy_perform(curl) : rc;
}
