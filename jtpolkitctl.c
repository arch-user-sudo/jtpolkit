/* jtpolkitctl — send a command to a running jtpolkit authentication agent.
 *
 * Usage:
 *   jtpolkitctl accent '#RRGGBB'   change the accent live
 *   jtpolkitctl '#RRGGBB'          shorthand for the same
 *   jtpolkitctl quit               unregister and exit the agent
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static int
ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0]) rt = "/tmp";
    int len = snprintf(buf, n, "%s/jtpolkit-ctl.sock", rt);
    return len >= 0 && (size_t)len < n;
}

static int
hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int
parse_color(const char *s, unsigned *out)
{
    if (!s)
        return 0;
    if (*s == '#')
        s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (strlen(s) != 6)
        return 0;

    unsigned value = 0;
    for (int i = 0; i < 6; i++) {
        int nibble = hex_nibble(s[i]);
        if (nibble < 0)
            return 0;
        value = (value << 4) | (unsigned)nibble;
    }
    *out = value;
    return 1;
}

static void
usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s accent '#RRGGBB'|quit\n"
            "       %s '#RRGGBB'\n"
            "  accent COLOR   change the window accent while the agent is running\n"
            "  quit           unregister the agent and exit\n",
            prog, prog);
}

int
main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    char msg[64];
    unsigned rgb;
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    } else if (!strcmp(argv[1], "accent")) {
        if (argc != 3 || !parse_color(argv[2], &rgb)) {
            fprintf(stderr, "%s: accent requires a color like '#378484'\n",
                    argv[0]);
            return 2;
        }
        snprintf(msg, sizeof(msg), "accent #%06X\n", rgb);
    } else if (argc == 2 && parse_color(argv[1], &rgb)) {
        snprintf(msg, sizeof(msg), "accent #%06X\n", rgb);
    } else if (!strcmp(argv[1], "quit")) {
        if (argc != 2) {
            usage(argv[0]);
            return 1;
        }
        snprintf(msg, sizeof(msg), "quit\n");
    } else {
        usage(argv[0]);
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("jtpolkitctl: socket");
        return 1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    char path[256];
    if (!ctl_path(path, sizeof path) ||
        strlen(path) >= sizeof addr.sun_path) {
        fprintf(stderr, "jtpolkitctl: control socket path too long\n");
        close(fd);
        return 1;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "jtpolkitctl: cannot connect to %s (is jtpolkit running?)\n",
                addr.sun_path);
        close(fd);
        return 1;
    }

    size_t len = strlen(msg);
    ssize_t sent;
    do {
        sent = send(fd, msg, len, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0 || (size_t)sent != len) {
        close(fd);
        return 1;
    }

    /* The daemon acknowledges processed commands.  Without this, a second
     * client being dropped by the single-slot server still looks successful. */
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    int pr;
    do {
        pr = poll(&pfd, 1, 2000);
    } while (pr < 0 && errno == EINTR);
    if (pr <= 0) {
        close(fd);
        return 1;
    }
    char ack = 0;
    ssize_t got;
    do {
        got = recv(fd, &ack, 1, 0);
    } while (got < 0 && errno == EINTR);
    close(fd);
    return (got == 1 && ack == 'A') ? 0 : 1;
}
