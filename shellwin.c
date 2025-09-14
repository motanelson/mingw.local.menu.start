/* server_win.c - versão Windows (MinGW)
 * Compilar (exemplo):
 *   gcc -O2 -Wall -o progman_server.exe server_win.c -lws2_32
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#pragma comment(lib, "ws2_32.lib")

#define BACKLOG 10
#define BUF_SIZE 8192
#define SMALL 256

typedef struct {
    char *caption;
    char *command;
} MenuItem;

typedef struct {
    MenuItem *items;
    size_t count;
} Menu;

static int keep_running = 1;

char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *r = _strdup(s);
    if (!r) { perror("strdup"); exit(1); }
    return r;
}

void trim_newline(char *s) {
    if (!s) return;
    size_t i = strlen(s);
    while (i && (s[i-1] == '\n' || s[i-1] == '\r')) { s[--i] = '\0'; }
}
int asprintf(char **strp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    // calcula o tamanho necessário
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (len < 0) return -1;

    *strp = (char *)malloc(len + 1);
    if (!*strp) return -1;

    va_start(ap, fmt);
    vsnprintf(*strp, len + 1, fmt, ap);
    va_end(ap);

    return len;
}
void load_progman_ini(const char *path, Menu *menu) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Aviso: não foi possível abrir %s\n", path);
        menu->items = NULL;
        menu->count = 0;
        return;
    }
    char line[1024];
    menu->items = NULL;
    menu->count = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char *sep = strchr(line, '|');
        if (!sep) continue;
        *sep = '\0';
        char *cap = line;
        char *cmd = sep + 1;
        if (strlen(cap) == 0 || strlen(cmd) == 0) continue;
        menu->items = realloc(menu->items, sizeof(MenuItem) * (menu->count + 1));
        menu->items[menu->count].caption = xstrdup(cap);
        menu->items[menu->count].command = xstrdup(cmd);
        menu->count++;
    }
    fclose(f);
}

void free_menu(Menu *menu) {
    if (!menu) return;
    for (size_t i = 0; i < menu->count; ++i) {
        free(menu->items[i].caption);
        free(menu->items[i].command);
    }
    free(menu->items);
    menu->items = NULL;
    menu->count = 0;
}

void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && isxdigit(a) && isxdigit(b)) {
            char hex[3] = {a, b, 0};
            *dst++ = (char) strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

char *run_command_capture(const char *cmd) {
    FILE *fp;
    size_t cap = 16384;
    size_t used = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';

    char *fullcmd;
    if (asprintf(&fullcmd, "cmd.exe /C %s", cmd) < 0) {
        free(out);
        return NULL;
    }

    fp = _popen(fullcmd, "r");
    free(fullcmd);
    if (!fp) {
        snprintf(out, cap, "Erro ao executar comando.\n");
        return out;
    }
    char buf[512];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t bl = strlen(buf);
        if (used + bl + 1 > cap) {
            cap *= 2;
            out = realloc(out, cap);
        }
        memcpy(out + used, buf, bl);
        used += bl;
        out[used] = '\0';
    }
    _pclose(fp);
    return out;
}

void send_simple(SOCKET fd, const char *status, const char *body) {
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %s\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n%s",
                     status, strlen(body), body);
    send(fd, header, n, 0);
}

int is_local_addr(struct sockaddr_storage *addr) {
    if (!addr) return 0;
    if (addr->ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in*)addr;
        return (ntohl(a->sin_addr.s_addr) >> 24) == 127;
    } else if (addr->ss_family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6*)addr;
        const unsigned char *b = (const unsigned char*)&a6->sin6_addr;
        for (int i=0;i<15;i++) if (b[i]!=0) return 0;
        return b[15]==1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <porta>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup falhou\n");
        return 1;
    }

    Menu menu = {0};
    load_progman_ini("progman.ini", &menu);

    SOCKET listenfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (listenfd == INVALID_SOCKET) {
        fprintf(stderr, "Erro socket\n");
        return 1;
    }

    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on));

    struct sockaddr_in6 serv6;
    memset(&serv6, 0, sizeof(serv6));
    serv6.sin6_family = AF_INET6;
    serv6.sin6_addr = in6addr_any;
    serv6.sin6_port = htons(port);

    if (bind(listenfd, (struct sockaddr*)&serv6, sizeof(serv6)) == SOCKET_ERROR) {
        fprintf(stderr, "Erro bind\n");
        closesocket(listenfd);
        WSACleanup();
        return 1;
    }
    listen(listenfd, BACKLOG);

    printf("Servidor em http://127.0.0.1:%d/\n", port);

    while (keep_running) {
        struct sockaddr_storage client_addr;
        int addrlen = sizeof(client_addr);
        SOCKET fd = accept(listenfd, (struct sockaddr*)&client_addr, &addrlen);
        if (fd == INVALID_SOCKET) continue;

        if (!is_local_addr(&client_addr)) {
            send_simple(fd, "403 Forbidden", "Apenas localhost permitido\n");
            closesocket(fd);
            continue;
        }

        char buf[BUF_SIZE];
        int r = recv(fd, buf, sizeof(buf)-1, 0);
        if (r <= 0) { closesocket(fd); continue; }
        buf[r] = 0;

        if (strncmp(buf, "GET / ", 6) == 0) {
            const char *resp =
                "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                "<html><body bgcolor=yellow><h1>Menu</h1>"
                "<form method='POST' action='/run'>"
                "Comando: <input name='cmd'><input type='submit'></form></body></html>";
            send(fd, resp, strlen(resp), 0);
        } else if (strncmp(buf, "POST /run", 9) == 0) {
            char *p = strstr(buf, "\r\n\r\n");
            if (!p) { closesocket(fd); continue; }
            p += 4;
            char *cmdpos = strstr(p, "cmd=");
            char dec[1024];
            if (cmdpos) {
                url_decode(dec, cmdpos+4);
                char *out = run_command_capture(dec);
                if (!out) out = xstrdup("Erro.\n");
                char hdr[512];
                snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                         "<html><body bgcolor=yellow><pre>%s</pre></body></html>", out);
                send(fd, hdr, strlen(hdr), 0);
                free(out);
            }
        } else {
            send_simple(fd, "404 Not Found", "Nao encontrado\n");
        }
        closesocket(fd);
    }

    closesocket(listenfd);
    WSACleanup();
    return 0;
}

