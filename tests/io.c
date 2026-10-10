/* The restricted test host permits local IPC read/write, but blocks send/recv.
 * Only the test HTTP transport uses these socket pairs. */
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

ssize_t send(int fd, const void *data, size_t size, int flags)
{
    (void)flags; return write(fd, data, size);
}

ssize_t recv(int fd, void *data, size_t size, int flags)
{
    if (flags & MSG_PEEK) { errno = EAGAIN; return -1; }
    return read(fd, data, size);
}

ssize_t sendto(int fd, const void *data, size_t size, int flags,
               const struct sockaddr *address, socklen_t length)
{
    (void)length;
    if (address) { errno = EOPNOTSUPP; return -1; }
    return send(fd, data, size, flags);
}

ssize_t recvfrom(int fd, void *data, size_t size, int flags,
                 struct sockaddr *address, socklen_t *length)
{
    (void)address; (void)length; return recv(fd, data, size, flags);
}
