/* nc -- a socket in the terminal.
 *
 *   nc [-u] [-C] HOST PORT     connect, and pass the terminal through
 *   nc -l [-u] [-C] PORT       wait for one to come in, then the same
 *
 * What arrives is printed as it comes (a thread of its own does that); what
 * is typed goes out a line at a time. -C ends lines with CR LF, which is what
 * HTTP, SMTP and the like want. -u is UDP instead of TCP. The end of input
 * (a file or pipe running out) closes our side and waits for the other. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <thread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static int en;
#define T(ru, eng) (en ? (eng) : (ru))

static int fd = -1, udp, listen_mode, crlf;
static volatile int peer_known, closed;
static struct sockaddr_in peer;

static void receiver(void *arg) {
    (void)arg;
    static char buf[4096];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = (int)recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 && !udp) break;
        if (n < 0) break;
        if (udp && listen_mode && !peer_known) {
            /* The first datagram says who we are talking to. */
            peer = from;
            connect(fd, (struct sockaddr *)&from, sizeof from);
            peer_known = 1;
        }
        write(1, buf, (unsigned long)n);
    }
    closed = 1;
    printf("%s", T("\n[соединение закрыто]\n", "\n[connection closed]\n"));
}

static int usage(void) {
    printf("%s", T("использование: nc [-u] [-C] АДРЕС ПОРТ   |   nc -l [-u] [-C] ПОРТ\n",
                   "usage: nc [-u] [-C] HOST PORT   |   nc -l [-u] [-C] PORT\n"));
    return 2;
}

int main(int argc, char **argv) {
    en = ui_lang() == 1;
    const char *host = 0, *port = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) listen_mode = 1;
        else if (!strcmp(argv[i], "-u")) udp = 1;
        else if (!strcmp(argv[i], "-C")) crlf = 1;
        else if (argv[i][0] == '-' && argv[i][1]) return usage();
        else if (!host && !listen_mode) host = argv[i];
        else if (!port) port = argv[i];
        else return usage();
    }
    if (!port || (!listen_mode && !host)) return usage();

    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_flags = listen_mode ? AI_PASSIVE : 0;
    int e = getaddrinfo(listen_mode ? 0 : host, port, &hints, &res);
    if (e) { printf("nc: %s: %s\n", host ? host : port, gai_strerror(e)); return 1; }

    fd = socket(AF_INET, hints.ai_socktype, 0);
    if (fd < 0) { printf("nc: socket: %s\n", strerror(errno)); return 1; }

    if (listen_mode) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, res->ai_addr, res->ai_addrlen) < 0) {
            printf("nc: bind %s: %s\n", port, strerror(errno));
            return 1;
        }
        if (!udp) {
            listen(fd, 1);
            printf(T("жду соединения на порту %s...\n", "listening on port %s...\n"), port);
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int c = accept(fd, (struct sockaddr *)&from, &fl);
            if (c < 0) { printf("nc: accept: %s\n", strerror(errno)); return 1; }
            close(fd);
            fd = c;
            char a[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, a, sizeof a);
            printf(T("подключились с %s:%d\n", "connection from %s:%d\n"), a, ntohs(from.sin_port));
        } else {
            printf(T("жду датаграмм на порту %s...\n", "waiting for datagrams on port %s...\n"), port);
        }
    } else {
        if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
            printf("nc: %s:%s: %s\n", host, port, strerror(errno));
            return 1;
        }
        peer = *(struct sockaddr_in *)res->ai_addr;
        peer_known = 1;
        if (!udp) {
            char a[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &peer.sin_addr, a, sizeof a);
            printf(T("соединено с %s (%s) порт %s\n", "connected to %s (%s) port %s\n"), host, a, port);
        }
    }
    freeaddrinfo(res);

    int tid = thread_create(receiver, 0);
    if (tid < 0) { printf("nc: thread: %s\n", strerror(errno)); return 1; }

    static char line[4096 + 2];
    for (;;) {
        long n = read(0, line, 4096);
        if (n <= 0 || closed) break;
        if (crlf && line[n - 1] == '\n' && (n < 2 || line[n - 2] != '\r')) {
            line[n - 1] = '\r';
            line[n++] = '\n';
        }
        if (udp && listen_mode && !peer_known) {
            printf("%s", T("nc: ещё никто не написал -- некому отвечать\n", "nc: nobody has written yet\n"));
            continue;
        }
        if (send(fd, line, (size_t)n, 0) < 0) {
            printf("nc: %s\n", strerror(errno));
            break;
        }
    }
    if (!udp && !closed) {
        shutdown(fd, SHUT_WR);
        thread_join(tid);
    }
    close(fd);
    return 0;
}
