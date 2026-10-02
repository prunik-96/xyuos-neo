/* httpd -- a small web server: the files in one folder, over HTTP/1.0.
 *
 *   httpd [PORT] [FOLDER]        (8080 and /home if not given)
 *
 * One request at a time, each answered and closed. A folder is shown as a
 * list of what is in it (or its index.html, if it has one). Nothing outside
 * the folder can be asked for: a path with ".." in it is refused. Each
 * request is written to the terminal. Ctrl+C stops it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int en;
#define T(ru, eng) (en ? (eng) : (ru))

static char root[512];

static const char *mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    static const char *map[][2] = {
        { ".html", "text/html; charset=utf-8" }, { ".htm", "text/html; charset=utf-8" },
        { ".txt", "text/plain; charset=utf-8" }, { ".c", "text/plain; charset=utf-8" },
        { ".h", "text/plain; charset=utf-8" }, { ".md", "text/plain; charset=utf-8" },
        { ".css", "text/css" }, { ".js", "text/javascript" }, { ".json", "application/json" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
        { ".gif", "image/gif" }, { ".svg", "image/svg+xml" }, { ".bmp", "image/bmp" },
        { ".ico", "image/x-icon" }, { ".mp3", "audio/mpeg" }, { ".wav", "audio/wav" },
        { ".pdf", "application/pdf" }, { ".zip", "application/zip" },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(dot, map[i][0])) return map[i][1];
    return "application/octet-stream";
}

static int send_all(int fd, const void *p, int n) {
    const char *c = p;
    while (n > 0) {
        int w = (int)send(fd, c, (size_t)n, 0);
        if (w <= 0) return -1;
        c += w;
        n -= w;
    }
    return 0;
}

static void reply(int fd, int code, const char *what, const char *type, long len, int head) {
    char h[512];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.0 %d %s\r\nServer: xyuOS-httpd\r\nContent-Type: %s\r\n"
                     "Content-Length: %ld\r\nConnection: close\r\n\r\n",
                     code, what, type, len);
    send_all(fd, h, n);
    (void)head;
}

static void error_page(int fd, int code, const char *what, int head) {
    char body[256];
    int n = snprintf(body, sizeof body, "<!doctype html><title>%d</title><h1>%d %s</h1>\n", code, code, what);
    reply(fd, code, what, "text/html; charset=utf-8", n, head);
    if (!head) send_all(fd, body, n);
}

/* %41 -> 'A', and the query string cut off. */
static void url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p && *p != '?' && *p != '#'; p++) {
        if (*p == '%' && p[1] && p[2]) {
            int v = 0;
            for (int k = 1; k <= 2; k++) {
                char c = p[k];
                v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 0);
            }
            *o++ = (char)v;
            p += 2;
        } else {
            *o++ = *p == '+' ? ' ' : *p;
        }
    }
    *o = 0;
}

static long listing(int fd, const char *dir, const char *urlpath, int head) {
    /* Built in memory first: the length goes in the header. */
    size_t cap = 65536, n = 0;
    char *b = malloc(cap);
    if (!b) { error_page(fd, 500, "Internal Server Error", head); return 0; }
    n += (size_t)snprintf(b + n, cap - n,
                          "<!doctype html><meta charset=utf-8><title>%s</title>"
                          "<style>body{font-family:sans-serif;margin:2em}a{text-decoration:none}</style>"
                          "<h1>%s</h1><ul>", urlpath, urlpath);
    if (strcmp(urlpath, "/")) n += (size_t)snprintf(b + n, cap - n, "<li><a href=\"..\">..</a>");
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < cap - 600) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        n += (size_t)snprintf(b + n, cap - n, "<li><a href=\"%s%s\">%s%s</a>",
                              e->d_name, isdir ? "/" : "", e->d_name, isdir ? "/" : "");
    }
    if (d) closedir(d);
    n += (size_t)snprintf(b + n, cap - n, "</ul><p><small>xyuOS httpd</small>\n");
    reply(fd, 200, "OK", "text/html; charset=utf-8", (long)n, head);
    if (!head) send_all(fd, b, (int)n);
    free(b);
    return head ? 0 : (long)n;
}

static void serve(int fd, const struct sockaddr_in *from) {
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    static char req[8192];
    int got = 0;
    while (got < (int)sizeof req - 1) {
        int r = (int)recv(fd, req + got, sizeof req - 1 - (size_t)got, 0);
        if (r <= 0) break;
        got += r;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    req[got] = 0;
    char method[8], path[1024];
    int code = 400;
    long bytes = 0;
    if (sscanf(req, "%7s %1023s", method, path) != 2) {
        error_page(fd, 400, "Bad Request", 0);
        method[0] = 0; path[0] = 0;
    } else {
        int head = !strcmp(method, "HEAD");
        url_decode(path);
        if (strcmp(method, "GET") && !head) {
            code = 501;
            error_page(fd, 501, "Not Implemented", 0);
        } else if (path[0] != '/' || strstr(path, "..")) {
            code = 403;
            error_page(fd, 403, "Forbidden", head);
        } else {
            char full[1024];
            snprintf(full, sizeof full, "%s%s", strcmp(root, "/") ? root : "", path);
            size_t fl = strlen(full);
            if (fl > 1 && full[fl - 1] == '/') full[fl - 1] = 0;
            struct stat st;
            if (stat(full[0] ? full : "/", &st) != 0) {
                code = 404;
                error_page(fd, 404, "Not Found", head);
            } else if (S_ISDIR(st.st_mode)) {
                char idx[1100];
                snprintf(idx, sizeof idx, "%s/index.html", full);
                struct stat is;
                if (path[strlen(path) - 1] != '/') {
                    /* So that the links inside it point into it. */
                    char h[1200];
                    int n = snprintf(h, sizeof h, "HTTP/1.0 301 Moved Permanently\r\nLocation: %s/\r\n"
                                     "Content-Length: 0\r\nConnection: close\r\n\r\n", path);
                    send_all(fd, h, n);
                    code = 301;
                } else if (stat(idx, &is) == 0 && !S_ISDIR(is.st_mode)) {
                    snprintf(full, sizeof full, "%s", idx);
                    st = is;
                    goto file;
                } else {
                    code = 200;
                    bytes = listing(fd, full[0] ? full : "/", path, head);
                }
            } else {
            file:;
                FILE *f = fopen(full, "rb");
                if (!f) {
                    code = 404;
                    error_page(fd, 404, "Not Found", head);
                } else {
                    code = 200;
                    reply(fd, 200, "OK", mime(full), (long)st.st_size, head);
                    if (!head) {
                        static char buf[16384];
                        size_t n;
                        while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
                            if (send_all(fd, buf, (int)n) < 0) break;
                            bytes += (long)n;
                        }
                    }
                    fclose(f);
                }
            }
        }
    }
    char a[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from->sin_addr, a, sizeof a);
    printf("%s  %s %s  %d  %ld %s\n", a, method, path, code, bytes, T("байт", "bytes"));
    close(fd);
}

int main(int argc, char **argv) {
    en = ui_lang() == 1;
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    const char *dir = argc > 2 ? argv[2] : "/home";
    if (port <= 0 || port > 65535) {
        printf("%s", T("использование: httpd [ПОРТ] [ПАПКА]\n", "usage: httpd [PORT] [FOLDER]\n"));
        return 2;
    }
    /* A relative folder is relative to where the shell is. */
    if (dir[0] != '/') {
        const char *pwd = getenv("PWD");
        snprintf(root, sizeof root, "%s/%s", pwd && *pwd ? pwd : "", dir);
    } else {
        snprintf(root, sizeof root, "%s", dir);
    }
    size_t rl = strlen(root);
    while (rl > 1 && root[rl - 1] == '/') root[--rl] = 0;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 8) < 0) {
        printf("httpd: %d: %s\n", port, strerror(errno));
        return 1;
    }
    printf(T("httpd: раздаю %s на порту %d (Ctrl+C - стоп)\n", "httpd: serving %s on port %d (Ctrl+C stops)\n"),
           root, port);
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int c = accept(s, (struct sockaddr *)&from, &fl);
        if (c < 0) {
            if (errno == EINTR) continue;
            printf("httpd: accept: %s\n", strerror(errno));
            break;
        }
        serve(c, &from);
    }
    close(s);
    return 0;
}
