// server.c - Minimal single-thread HTTP/1.1 static file server
// Windows (MinGW) build:
//   gcc -O2 -Wall -Wextra -std=c11 server.c -o server.exe -lws2_32
// Linux/macOS build (also works):
//   gcc -O2 -Wall -Wextra -std=c11 server.c -o server
//
// Serves files from ./public (creates it with an index.html if missing).
// Handles GET and HEAD. No pthreads, no sendfile.

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef _WIN32
  /* Avoid redef warning; MinGW already sets _WIN32_WINNT via sdkddkver.h */
  #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0601
  #endif

  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <direct.h>   // _mkdir
  #include <io.h>       // _open/_read/_close
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET socket_t
  ;
  #ifndef PATH_MAX
  #define PATH_MAX 260
  #endif
  #define close_socket(s) closesocket(s)
  #ifndef ssize_t
  #define ssize_t SSIZE_T
  #endif
  #ifndef O_BINARY
  #define O_BINARY 0
  #endif
  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
  #endif
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int socket_t;
  #define close_socket(s) close(s)
  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
  #endif
#endif

#define SERVER_NAME   "c-http/0.3"
#define DEFAULT_PORT  8080
#define DOC_ROOT      "public"
#define RECV_LIMIT    8192

static volatile sig_atomic_t g_running = 1;

/* ---------- utilities ---------- */

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void http_time_now(char *buf, size_t n) {
    time_t t = time(NULL);
#if defined(_WIN32)
    /* MinGW doesn’t have gmtime_s, use gmtime() */
    struct tm *ptm = gmtime(&t);
    if (!ptm) {
        snprintf(buf, n, "Thu, 01 Jan 1970 00:00:00 GMT");
        return;
    }
    struct tm tm = *ptm;
#else
    struct tm tm;
    gmtime_r(&t, &tm);
#endif
    strftime(buf, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static const char* mime_from_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript; charset=utf-8";
    if (!strcmp(dot, ".json")) return "application/json; charset=utf-8";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".gif"))  return "image/gif";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".txt"))  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static void send_fmt(socket_t fd, const char *fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
#ifdef _WIN32
        send(fd, buf, n, 0);
#else
        send(fd, buf, (size_t)n, MSG_NOSIGNAL);
#endif
    }
}

static void send_error(socket_t cfd, int status, const char *reason) {
    char date[64]; http_time_now(date, sizeof(date));
    const char *tmpl =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>%d %s</title></head>"
        "<body><h1>%d %s</h1></body></html>";
    char body[512];
    int blen = snprintf(body, sizeof(body), tmpl, status, reason, status, reason);

    send_fmt(cfd,
        "HTTP/1.1 %d %s\r\n"
        "Date: %s\r\n"
        "Server: %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        status, reason, date, SERVER_NAME, blen);

#ifdef _WIN32
    send(cfd, body, blen, 0);
#else
    send(cfd, body, (size_t)blen, MSG_NOSIGNAL);
#endif
}

static bool safe_join(char *out, size_t n, const char *root, const char *rel) {
    if (!out || !root || !rel) return false;
    if (strstr(rel, "..")) return false;
    while (*rel == '/') rel++;              // strip leading /

    if (*rel == '\0') {
        return snprintf(out, n, "%s/index.html", root) < (int)n;
    }
    size_t len = strlen(rel);
    if (len > 0 && rel[len-1] == '/') {
        return snprintf(out, n, "%s/%sindex.html", root, rel) < (int)n;
    }
    return snprintf(out, n, "%s/%s", root, rel) < (int)n;
}

/* ---------- request parsing ---------- */

struct request {
    char method[8];
    char target[2048];
    char version[16];
};

static bool parse_request_line(const char *line, struct request *req) {
    return sscanf(line, "%7s %2047s %15s", req->method, req->target, req->version) == 3;
}

/* ---------- client handling ---------- */

static void handle_client(socket_t cfd) {
    char buf[RECV_LIMIT + 1];
#ifdef _WIN32
    int r = recv(cfd, buf, RECV_LIMIT, 0);
#else
    int r = recv(cfd, buf, RECV_LIMIT, MSG_NOSIGNAL);
#endif
    if (r <= 0) { close_socket(cfd); return; }
    buf[r] = '\0';

    char *line_end = strstr(buf, "\r\n");
    if (!line_end) { send_error(cfd, 400, "Bad Request"); close_socket(cfd); return; }
    *line_end = '\0';

    struct request req;
    if (!parse_request_line(buf, &req)) {
        send_error(cfd, 400, "Bad Request"); close_socket(cfd); return;
    }

    bool is_get  = (strcmp(req.method, "GET")  == 0);
    bool is_head = (strcmp(req.method, "HEAD") == 0);
    if (!is_get && !is_head) {
        send_error(cfd, 405, "Method Not Allowed");
        close_socket(cfd);
        return;
    }

    char fs_path[PATH_MAX];
    if (!safe_join(fs_path, sizeof(fs_path), DOC_ROOT, req.target)) {
        send_error(cfd, 400, "Bad Request"); close_socket(cfd); return;
    }

    struct stat st;
    if (stat(fs_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_error(cfd, 404, "Not Found"); close_socket(cfd); return;
    }

    int fd;
#ifdef _WIN32
    fd = _open(fs_path, _O_RDONLY | O_BINARY);
#else
    fd = open(fs_path, O_RDONLY);
#endif
    if (fd < 0) {
        send_error(cfd, 403, "Forbidden"); close_socket(cfd); return;
    }

    const char *ctype = mime_from_path(fs_path);
    char date[64]; http_time_now(date, sizeof(date));

    send_fmt(cfd,
        "HTTP/1.1 200 OK\r\n"
        "Date: %s\r\n"
        "Server: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Connection: close\r\n\r\n",
        date, SERVER_NAME, ctype, (long long)st.st_size);

    if (is_get) {
        char fbuf[16384];
        for (;;) {
#ifdef _WIN32
            int nread = _read(fd, fbuf, (unsigned int)sizeof(fbuf));
#else
            ssize_t nread = read(fd, fbuf, sizeof(fbuf));
#endif
            if (nread < 0) break;
            if (nread == 0) break;

            const char *p = fbuf;
            int remaining = (int)nread;
            while (remaining > 0) {
                int nsent = send(cfd, p, remaining, 0);
                if (nsent <= 0) { remaining = -1; break; }
                p += nsent;
                remaining -= nsent;
            }
            if (remaining < 0) break;
        }
    }

#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
    close_socket(cfd);
}

/* ---------- server socket ---------- */

static socket_t make_server_socket(uint16_t port) {
    socket_t s = (socket_t)socket(AF_INET, SOCK_STREAM, 0);
    if ((intptr_t)s == -1) die("socket: %s", strerror(errno));

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        die("bind: %s", strerror(errno));

    if (listen(s, 128) < 0)
        die("listen: %s", strerror(errno));

    return s;
}

static int ensure_docroot(void) {
    struct stat st;
    if (stat(DOC_ROOT, &st) == 0 && (st.st_mode & S_IFDIR)) return 0;

#ifdef _WIN32
    if (_mkdir(DOC_ROOT) != 0) {
        fprintf(stderr, "mkdir '%s' failed: %s\n", DOC_ROOT, strerror(errno));
        return -1;
    }
#else
    if (mkdir(DOC_ROOT, 0755) != 0) {
        fprintf(stderr, "mkdir '%s' failed: %s\n", DOC_ROOT, strerror(errno));
        return -1;
    }
#endif
    FILE *f = fopen(DOC_ROOT "/index.html", "w");
    if (f) {
        fputs("<!doctype html><h1>It works!</h1>\n", f);
        fclose(f);
    }
    return 0;
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) die("WSAStartup failed");
#endif

    uint16_t port = (argc >= 2) ? (uint16_t)atoi(argv[1]) : DEFAULT_PORT;

    if (ensure_docroot() != 0) {
#ifdef _WIN32
        WSACleanup();
#endif
        return EXIT_FAILURE;
    }

    socket_t sfd = make_server_socket(port);
    printf("Serving %s on http://0.0.0.0:%u (Ctrl+C to quit)\n", DOC_ROOT, port);

    while (g_running) {
        struct sockaddr_in cli;
        socklen_t clilen = (socklen_t)sizeof(cli);
        socket_t cfd = accept(sfd, (struct sockaddr*)&cli, &clilen);
        if ((intptr_t)cfd < 0) {
            if (!g_running) break;
            perror("accept");
            continue;
        }
        /* Single-threaded: handle the client right here */
        handle_client(cfd);
    }

    close_socket(sfd);

#ifdef _WIN32
    WSACleanup();
#endif
    puts("\nShutting down.");
    return 0;
}
