/* socktest -- checks the sockets end to end.
 *
 * Over the loopback (no network needed): a TCP server and client in one
 * program, a megabyte each way and checked, shutdown and the end of stream,
 * a refused connection, timeouts, many connections opened and closed, UDP
 * datagrams and who sent them. Then, if there is a network: a name looked up,
 * a web page fetched over a socket, and a DNS question asked over UDP by hand.
 * Prints one line per check and a total. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static int passed, failed;

static void check(int ok, const char *what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (ok) passed++; else failed++;
}

static struct sockaddr_in loop_addr(int port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

static unsigned fnv(unsigned h, const unsigned char *p, int n) {
    for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* The byte at position i of the test stream `seed`. */
static unsigned char pat(unsigned seed, unsigned i) { return (unsigned char)((i * 31u + seed * 7u + (i >> 9)) & 0xFF); }

/* A connected pair over the loopback: *c the client, *s the server's end. */
static int pair(int lfd, int port, int *c, int *s) {
    *c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in a = loop_addr(port);
    int r = connect(*c, (struct sockaddr *)&a, sizeof a);
    if (r == 0 || errno != EINPROGRESS) return -1;
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    *s = accept(lfd, (struct sockaddr *)&peer, &pl);
    if (*s < 0) return -2;
    struct pollfd p = { *c, POLLOUT, 0 };
    if (poll(&p, 1, 2000) != 1 || !(p.revents & POLLOUT)) return -3;
    int err = -1;
    socklen_t el = sizeof err;
    getsockopt(*c, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) return -4;
    int fl = fcntl(*c, F_GETFL);
    fcntl(*c, F_SETFL, fl & ~O_NONBLOCK);
    return 0;
}

static void tcp_tests(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = loop_addr(5555);
    check(lfd >= 32, "socket() gives a descriptor");
    check(bind(lfd, (struct sockaddr *)&a, sizeof a) == 0, "bind 127.0.0.1:5555");
    check(listen(lfd, 4) == 0, "listen");

    int l2 = socket(AF_INET, SOCK_STREAM, 0);
    bind(l2, (struct sockaddr *)&a, sizeof a);
    check(listen(l2, 4) < 0 && errno == EADDRINUSE, "a second listener on the same port: EADDRINUSE");
    close(l2);

    int c, s;
    int pr = pair(lfd, 5555, &c, &s);
    check(pr == 0, "non-blocking connect -> EINPROGRESS, accept, poll says writable, SO_ERROR 0");
    if (pr) { printf("      (pair: %d, errno %d)\n", pr, errno); return; }

    struct sockaddr_in me, him;
    socklen_t ml = sizeof me, hl = sizeof him;
    getsockname(c, (struct sockaddr *)&me, &ml);
    getpeername(s, (struct sockaddr *)&him, &hl);
    check(me.sin_port == him.sin_port && ntohl(him.sin_addr.s_addr) == INADDR_LOOPBACK,
          "getsockname of the client = getpeername at the server");

    /* A megabyte each way, at the same time, in slices: neither side may
     * block the other. */
    const int N = 1024 * 1024;
    int sent_c = 0, sent_s = 0, got_c = 0, got_s = 0;
    unsigned hc = 2166136261u, hs = 2166136261u, want_c = 2166136261u, want_s = 2166136261u;
    static unsigned char buf[16384], out[16384];
    fcntl(c, F_SETFL, O_NONBLOCK);
    fcntl(s, F_SETFL, O_NONBLOCK);
    unsigned t0 = uptime_ms();
    int stuck = 0;
    while ((got_c < N || got_s < N) && stuck < 4000) {
        struct pollfd p[2] = { { c, POLLIN | (sent_c < N ? POLLOUT : 0), 0 },
                               { s, POLLIN | (sent_s < N ? POLLOUT : 0), 0 } };
        if (poll(p, 2, 1000) <= 0) { stuck += 1000; continue; }
        if (p[0].revents & POLLOUT) {
            int n = N - sent_c < (int)sizeof out ? N - sent_c : (int)sizeof out;
            for (int i = 0; i < n; i++) out[i] = pat(1, (unsigned)(sent_c + i));
            int w = (int)send(c, out, (size_t)n, 0);
            if (w > 0) { want_s = fnv(want_s, out, w); sent_c += w; }
        }
        if (p[1].revents & POLLOUT) {
            int n = N - sent_s < (int)sizeof out ? N - sent_s : (int)sizeof out;
            for (int i = 0; i < n; i++) out[i] = pat(2, (unsigned)(sent_s + i));
            int w = (int)send(s, out, (size_t)n, 0);
            if (w > 0) { want_c = fnv(want_c, out, w); sent_s += w; }
        }
        if (p[0].revents & POLLIN) {
            int r = (int)recv(c, buf, sizeof buf, 0);
            if (r > 0) { hc = fnv(hc, buf, r); got_c += r; }
        }
        if (p[1].revents & POLLIN) {
            int r = (int)recv(s, buf, sizeof buf, 0);
            if (r > 0) { hs = fnv(hs, buf, r); got_s += r; }
        }
    }
    unsigned ms = uptime_ms() - t0;
    char line[160];
    snprintf(line, sizeof line, "1 MB each way at once, both intact (%u ms, %u KB/s)",
             ms, ms ? (unsigned)(2u * 1024u * 1000u / ms) : 0u);
    check(got_c == N && got_s == N && hc == want_c && hs == want_s, line);

    fcntl(c, F_SETFL, 0);
    fcntl(s, F_SETFL, 0);

    /* The end of a stream. */
    shutdown(c, SHUT_WR);
    int r = (int)recv(s, buf, sizeof buf, 0);
    check(r == 0, "shutdown(SHUT_WR) -> the other end reads 0");
    send(s, "bye", 3, 0);
    close(s);
    r = (int)recv(c, buf, sizeof buf, MSG_WAITALL);
    check(r == 3 && !memcmp(buf, "bye", 3), "what was sent before close arrives");
    r = (int)recv(c, buf, sizeof buf, 0);
    check(r == 0, "and then the end");
    close(c);

    /* Nobody there. */
    int x = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in nob = loop_addr(5999);
    r = connect(x, (struct sockaddr *)&nob, sizeof nob);
    check(r < 0 && errno == ECONNREFUSED, "connect to a closed port: ECONNREFUSED");
    close(x);

    /* Waiting with a limit. */
    pr = pair(lfd, 5555, &c, &s);
    struct timeval tv = { 0, 300000 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    t0 = uptime_ms();
    r = (int)recv(c, buf, 10, 0);
    ms = uptime_ms() - t0;
    check(pr == 0 && r < 0 && errno == EAGAIN && ms >= 280 && ms < 1500, "SO_RCVTIMEO 300 ms -> EAGAIN in time");
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(c, &rs);
    struct timeval tv2 = { 0, 200000 };
    t0 = uptime_ms();
    int sel = select(c + 1, &rs, 0, 0, &tv2);
    ms = uptime_ms() - t0;
    check(sel == 0 && ms >= 180, "select with nothing to read -> 0 after its timeout");
    send(s, "x", 1, 0);
    FD_ZERO(&rs);
    FD_SET(c, &rs);
    tv2.tv_sec = 2; tv2.tv_usec = 0;
    sel = select(c + 1, &rs, 0, 0, &tv2);
    int avail = -1;
    ioctl(c, FIONREAD, &avail);
    check(sel == 1 && FD_ISSET(c, &rs) && avail == 1, "select sees data; FIONREAD says 1");
    close(c);
    close(s);

    /* Many, one after another: nothing may be left behind. */
    int ok = 1;
    for (int i = 0; i < 40 && ok; i++) {
        int pr2 = pair(lfd, 5555, &c, &s);
        if (pr2 != 0) { printf("      (connection %d: pair %d, errno %d)\n", i, pr2, errno); ok = 0; break; }
        long w = write(c, "ping", 4);
        char b4[4];
        long r4 = read(s, b4, 4);
        if (r4 != 4 || memcmp(b4, "ping", 4)) {
            printf("      (connection %d: write %ld, read %ld, errno %d)\n", i, w, r4, errno);
            ok = 0;
        }
        close(c);
        close(s);
    }
    check(ok, "40 connections opened, used and closed in turn");
    close(lfd);
}

static void udp_tests(void) {
    int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in aa = loop_addr(6001), ba = loop_addr(6002);
    check(bind(a, (struct sockaddr *)&aa, sizeof aa) == 0 && bind(b, (struct sockaddr *)&ba, sizeof ba) == 0,
          "UDP: bind two ports");
    for (int i = 0; i < 10; i++) {
        char m[32];
        int n = snprintf(m, sizeof m, "datagram %d", i);
        sendto(a, m, (size_t)n, 0, (struct sockaddr *)&ba, sizeof ba);
    }
    int avail = 0;
    char buf[64];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int r = (int)recvfrom(b, buf, sizeof buf, MSG_PEEK, (struct sockaddr *)&from, &fl);
    ioctl(b, FIONREAD, &avail);
    check(r == 10 && avail == 10 && ntohs(from.sin_port) == 6001, "MSG_PEEK and FIONREAD see the first, from :6001");
    int ok = 1;
    for (int i = 0; i < 10; i++) {
        char want[32];
        snprintf(want, sizeof want, "datagram %d", i);
        r = (int)recv(b, buf, sizeof buf - 1, 0);
        if (r < 0) { ok = 0; break; }
        buf[r] = 0;
        if (strcmp(buf, want)) ok = 0;
    }
    check(ok, "ten datagrams arrive whole and in order");
    /* connect() on a datagram socket sets where send() goes. */
    connect(b, (struct sockaddr *)&aa, sizeof aa);
    send(b, "back", 4, 0);
    r = (int)recv(a, buf, sizeof buf, 0);
    check(r == 4 && !memcmp(buf, "back", 4), "connected UDP: send() goes to the peer");
    close(a);
    close(b);
}

static void name_tests(void) {
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo("localhost", "http", &hints, &res);
    check(e == 0 && res && ((struct sockaddr_in *)res->ai_addr)->sin_port == htons(80) &&
          ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr) == INADDR_LOOPBACK,
          "getaddrinfo(localhost, http) = 127.0.0.1:80");
    if (res) freeaddrinfo(res);
    check(inet_addr("10.0.2.15") == htonl(0x0A00020F), "inet_addr");
}

static void net_tests(void) {
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo("example.com", "80", &hints, &res);
    check(e == 0 && res, "getaddrinfo(example.com) over the network");
    if (e) { printf("      (%s)\n", gai_strerror(e)); return; }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    check(r == 0, "connect to example.com:80");
    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\nAccept-Encoding: identity\r\n\r\n";
    send(fd, req, strlen(req), 0);
    static char page[65536];
    int got = 0;
    while (got < (int)sizeof page - 1) {
        int n = (int)recv(fd, page + got, sizeof page - 1 - (size_t)got, 0);
        if (n <= 0) break;
        got += n;
    }
    page[got] = 0;
    close(fd);
    char line[96];
    snprintf(line, sizeof line, "a web page over the socket (%d bytes)", got);
    check(!strncmp(page, "HTTP/1.", 7) && strstr(page, "Example Domain"), line);

    /* DNS by hand: an A question for example.com, to the name server, on
     * UDP port 53. */
    struct net_info ni;
    memset(&ni, 0, sizeof ni);
    if (net_config(&ni) != 0 || !ni.dns) { check(0, "know the name server"); return; }
    unsigned char q[64];
    int n = 0;
    q[n++] = 0x12; q[n++] = 0x34; q[n++] = 0x01; q[n++] = 0x00;
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    q[n++] = 7; memcpy(q + n, "example", 7); n += 7;
    q[n++] = 3; memcpy(q + n, "com", 3); n += 3;
    q[n++] = 0; q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dns;
    memset(&dns, 0, sizeof dns);
    dns.sin_family = AF_INET;
    dns.sin_port = htons(53);
    dns.sin_addr.s_addr = htonl(ni.dns);
    struct timeval tv = { 3, 0 };
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sendto(u, q, (size_t)n, 0, (struct sockaddr *)&dns, sizeof dns);
    unsigned char ans[512];
    r = (int)recv(u, ans, sizeof ans, 0);
    close(u);
    check(r > 12 && ans[0] == 0x12 && ans[1] == 0x34 && (ans[3] & 0x0F) == 0 && (ans[6] << 8 | ans[7]) > 0,
          "a DNS answer over UDP to the name server");
}

int main(int argc, char **argv) {
    int with_net = !(argc > 1 && !strcmp(argv[1], "-l"));
    printf("sockets: loopback\n");
    tcp_tests();
    udp_tests();
    name_tests();
    if (with_net) {
        printf("sockets: network\n");
        net_tests();
    }
    printf("%d ok, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
