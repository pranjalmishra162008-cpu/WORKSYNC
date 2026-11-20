
 * WorkSync - company workspace web app in pure C (no libraries).
 * Copyright (c) 2026 WorkSync. All rights reserved by PRANJAL MISHRA.
 *
 * Features:
 *   - Company registration with logo, browser-tab title, tagline and colours
 *   - Each company gets its own address:  https://worksync.com/w/<company-name>-pm
 *     (and <company-name>.PUBLIC_HOST / <company-name>.localhost if DNS points there)
 *   - Employees with post, salary, daily task target; company-issued login ID + password
 *   - Employee login: clock in/out, task log -> work time and efficiency tracking
 *   - Resources with photos, monthly bill (resources + payroll)
 *   - Growth & funds: funding received, revenue, expenses, cash balance, runway,
 *     month-over-month revenue growth, headcount
 *
 * Windows (MinGW-w64) : gcc -O2 -o cpbl.exe cpbl.c -lws2_32
 * Windows (VS prompt)  : cl /O2 cpbl.c
 * Linux / macOS        : gcc -O2 -o cpbl cpbl.c
 * Run   : cpbl
 *         Public address is set by PUBLIC_SCHEME / PUBLIC_HOST below.
 *         Put nginx/Caddy on ports 80/443 and forward to 127.0.0.1:8080
 *         (pass the Host header). Local testing: http://localhost:8080
 * Data  : stored in ./data
 * NOTE  : passwords use a simple hash and there is no HTTPS in this program. Put it
 *         behind a reverse proxy with TLS and swap the hash for bcrypt/argon2 before
 *         using it with real customers. Change SECRET below.
 */
#define _GNU_SOURCE
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#ifdef _WIN32                          /* ---- Windows (Winsock) ---- */
#include <winsock2.h>
#include <direct.h>
#pragma comment(lib, "ws2_32.lib")
#define MKDIR(p)  _mkdir(p)
#define CLOSESOCK closesocket
#define strncasecmp _strnicmp
static char *strcasestr(const char *h, const char *n) {
    size_t l = strlen(n);
    for (; *h; h++) if (!_strnicmp(h, n, l)) return (char *)h;
    return 0;
}
#else                                  /* ---- Linux / macOS ---- */
#include <unistd.h>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#define MKDIR(p)  mkdir(p, 0700)
#define CLOSESOCK close
#endif

#define PORT    8080
/* ---- Public address of WorkSync. Change PUBLIC_HOST to your real domain (no scheme). ---- */
#define PUBLIC_HOST   "worksync.com"   /* company URLs become https://worksync.com/w/<company-name>-pm */
#define PUBLIC_SCHEME "https"          /* needs a TLS certificate on your reverse proxy (nginx/Caddy) */
#define MAXBODY (8 * 1024 * 1024)
#define SECRET  "change-this-secret"
#define INITIALS "pm"                  /* owner initials (PRANJAL MISHRA), added to every public link */
#define APPNAME "WorkSync"
static const char *CAT[] = { "", "Equipment", "Software", "Space", "People" };

typedef struct { int id; char name[64], email[96], slug[48]; unsigned long ph; } User;
typedef struct { int id, cat, qty, photo; double cost; char name[64], month[8]; } Res;
typedef struct { int id, type; double amt; char month[8], note[64]; } Fund;   /* type 1 funding, 2 revenue */

static char out[1 << 17]; static int ol;
static char g_host[100];
static void ap(const char *f, ...) {
    va_list a; va_start(a, f);
    if (ol < (int)sizeof out - 1) ol += vsnprintf(out + ol, sizeof out - ol, f, a);
    va_end(a);
}
static void esc(const char *s) {
    for (; *s; s++) switch (*s) {
        case '<': ap("&lt;"); break; case '>': ap("&gt;"); break;
        case '&': ap("&amp;"); break; case '"': ap("&quot;"); break;
        case '\'': ap("&#39;"); break;
        default: ap("%c", *s);
    }
}
static unsigned long hs(const char *s) {
    unsigned long h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h;
}
static void clean(char *s) { for (; *s; s++) if (*s == '|' || *s == '\n' || *s == '\r') *s = ' '; }
static void urldec(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; *s && i + 1 < n; s++, i++) {
        if (*s == '+') d[i] = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 }; d[i] = (char)strtol(h, 0, 16); s += 2;
        } else d[i] = *s;
    }
    d[i] = 0;
}
static int param(const char *b, const char *k, char *o, size_t n) {
    size_t kl = strlen(k); o[0] = 0;
    while (*b) {
        if (!strncmp(b, k, kl) && b[kl] == '=') {
            const char *v = b + kl + 1; size_t l = strcspn(v, "&");
            char *t = malloc(l + 1); memcpy(t, v, l); t[l] = 0;
            urldec(o, t, n); free(t); clean(o); return 1;
        }
        b += strcspn(b, "&"); if (*b) b++;