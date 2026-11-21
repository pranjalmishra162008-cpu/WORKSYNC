/*
 * WorkSync - company workspace web app in pure C (no libraries).
 * Copyright (c) 2026 WorkSync. All rights reserved by PRANJAL MISHRA.
 *
 * Features:
 *   - Company registration with logo, browser-tab title, tagline and colours
 *   - Each company gets its own address:  https://<your-host>/w/<company-name>-pm
 *   - Employees with post, salary, daily task target; company-issued login ID + password
 *   - Custom employee Login ID (letters, numbers and @ only) set by the owner
 *   - Employee login: clock in/out, task log -> work time and efficiency tracking
 *   - Goals: the owner sets goals for each employee separately; they show on the employee's screen
 *   - Meetings: the owner schedules meetings; employees get an alert 15 minutes before
 *   - Resources with photos, monthly bill (resources + payroll)
 *   - Growth & funds: funding received, revenue, expenses, cash balance, runway
 *   - Contact us page (company address, phone, email)
 *   - Theme presets, light/dark/cream modes, corner and header styles
 *   - Site font and company-name font, case, size, weight and slant
 *
 * Windows (MinGW-w64) : gcc -O2 -o cpbl.exe cpbl.c -lws2_32
 * Windows (VS prompt)  : cl /O2 cpbl.c
 * Linux / macOS        : gcc -O2 -o cpbl cpbl.c
 * Run   : cpbl          (listens on port 8080)
 *         Links shown to the owner are built from the address the browser used,
 *         so PUBLIC_HOST below is only a fallback.
 * Data  : stored in ./data
 * NOTE  : passwords use a simple hash. Put it behind a reverse proxy with TLS and
 *         swap the hash for bcrypt/argon2 before using it with real customers.
 *         Change SECRET below.
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
/* Fallback only (used when a request has no Host header). */
#define PUBLIC_HOST   "localhost"
#define PUBLIC_SCHEME "https"
#define MAXBODY (8 * 1024 * 1024)
#define SECRET  "change-this-secret"
#define INITIALS "pm"                  /* owner initials (PRANJAL MISHRA), added to every public link */
#define APPNAME "WorkSync"
#define ALERT_SECS 900                 /* employees are alerted this many seconds before a meeting (15 min) */
static const char *CAT[] = { "", "Equipment", "Software", "Space", "People" };

/* ---- look and feel options ---- */
static const char *FONTS[]  = {
    "'Helvetica Neue',Arial,sans-serif",
    "Georgia,'Times New Roman',serif",
    "'Playfair Display','Cormorant Garamond',Georgia,serif",
    "'Trebuchet MS','Segoe UI',Verdana,sans-serif",
    "'Courier New',Consolas,monospace",
    "'Brush Script MT','Segoe Script',cursive" };
static const char *FNAMES[] = { "Modern sans", "Classic serif", "Elegant serif", "Friendly sans", "Typewriter", "Handwritten" };
static const char *CASES[]  = { "none", "uppercase", "lowercase", "capitalize" };
static const char *CASEN[]  = { "As typed", "UPPERCASE", "lowercase", "Title Case" };
static const char *SIZES[]  = { "20px", "26px", "34px", "44px" };
static const char *SIZEN[]  = { "Small", "Medium", "Large", "Extra large" };
static const char *MODEN[]  = { "Light", "Dark", "Warm cream" };
static const char *RADN[]   = { "Sharp corners", "Soft corners", "Round and friendly" };
static const char *HDRN[]   = { "Solid colour", "Colour gradient", "Light and clean" };
typedef struct { const char *name, *pri, *acc; int mode; } Preset;
static const Preset PRE[] = {
    { "Midnight Gold", "#12203a", "#a8802f", 0 }, { "Ocean",       "#0b3c5d", "#1d9bd1", 0 },
    { "Forest",        "#14452f", "#7fb927", 0 }, { "Sunset",      "#5b1e3a", "#ff7a45", 0 },
    { "Violet",        "#2e1a5e", "#9b6bff", 0 }, { "Night Mint",  "#1f2933", "#2ec4a6", 1 },
    { "Rose Cream",    "#7a1f3d", "#e85d8a", 2 }, { "Graphite",    "#111111", "#4b5563", 0 } };

typedef struct { int id; char name[64], email[96], slug[48]; unsigned long ph; } User;
typedef struct { int id, cat, qty, photo; double cost; char name[64], month[8]; } Res;
typedef struct { int id, type; double amt; char month[8], note[64]; } Fund;   /* type 1 funding, 2 revenue */

static char out[1 << 17]; static int ol;
static char g_host[100], g_proto[8];
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
    }
    return 0;
}
static int ci(const char *s, int hi) { int v = atoi(s); return v < 0 ? 0 : v > hi ? hi : v; }
static void slugify(const char *s, char *o, size_t n) {
    size_t i = 0; int dash = 0;
    for (; *s && i + 1 < n; s++) {
        if (isalnum((unsigned char)*s)) { o[i++] = (char)tolower((unsigned char)*s); dash = 0; }
        else if (i && !dash) { o[i++] = '-'; dash = 1; }
    }
    if (i && o[i - 1] == '-') i--;
    o[i] = 0; if (!i) strcpy(o, "company");
}
static void trim(char *s) {
    size_t l; char *p = s;
    while (*p == ' ') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    l = strlen(s); while (l && s[l - 1] == ' ') s[--l] = 0;
}
static int lideq(const char *a, const char *b) {          /* case-insensitive compare */
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}
/* Custom login ID rule: 3-30 characters, only letters, digits and @, at least one letter. */
static int valid_lid(const char *s) {
    size_t l = strlen(s), i; int letter = 0;
    if (l < 3 || l > 30) return 0;
    for (i = 0; i < l; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) letter = 1;
        else if (!(c >= '0' && c <= '9') && c != '@') return 0;
    }
    return letter;
}
static int okdate(const char *s) {                         /* YYYY-MM-DD */
    int i; if (strlen(s) != 10) return 0;
    for (i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return 0; }
        else if (!isdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

/* ---------- storage ---------- */
/* users.txt line: id|name|email|hash|slug   (older 4-field lines get slug "c<id>") */
static int load_user(int id, const char *email, const char *slug, User *u) {
    FILE *f = fopen("data/users.txt", "r"); char ln[400]; if (!f) return 0;
    while (fgets(ln, sizeof ln, f)) {
        User t; int k; memset(&t, 0, sizeof t);
        k = sscanf(ln, "%d|%63[^|]|%95[^|]|%lu|%47s", &t.id, t.name, t.email, &t.ph, t.slug);
        if (k < 4) continue;
        if (k == 4) sprintf(t.slug, "c%d", t.id);
        if ((id && t.id == id) || (email && !strcmp(t.email, email)) || (slug && !strcmp(t.slug, slug))) {
            *u = t; fclose(f); return 1;
        }
    }
    fclose(f); return 0;
}
static int next_user_id(void) {
    FILE *f = fopen("data/users.txt", "r"); char ln[400]; int m = 0, i;
    if (f) { while (fgets(ln, sizeof ln, f)) if (sscanf(ln, "%d", &i) == 1 && i > m) m = i; fclose(f); }
    return m + 1;
}
static char *rpath(int uid, char *p) { sprintf(p, "data/res_%d.txt", uid); return p; }
static int load_res(int uid, Res *r, int max) {
    char p[64], ln[300]; int n = 0; FILE *f = fopen(rpath(uid, p), "r"); if (!f) return 0;
    while (n < max && fgets(ln, sizeof ln, f))
        if (sscanf(ln, "%d|%63[^|]|%d|%d|%lf|%7[^|]|%d", &r[n].id, r[n].name, &r[n].cat,
                   &r[n].qty, &r[n].cost, r[n].month, &r[n].photo) == 7) n++;
    fclose(f); return n;
}
static void save_res(int uid, Res *r, int n) {
    char p[64]; FILE *f = fopen(rpath(uid, p), "w"); int i; if (!f) return;
    for (i = 0; i < n; i++)
        fprintf(f, "%d|%s|%d|%d|%.2f|%s|%d\n", r[i].id, r[i].name, r[i].cat, r[i].qty,
                r[i].cost, r[i].month, r[i].photo);
    fclose(f);
}
static void token(const User *u, char *t) { char b[200]; sprintf(b, "%d%lu%s", u->id, u->ph, SECRET); sprintf(t, "%lx", hs(b)); }
static int auth(const char *hdr, User *u) {
    const char *c = strcasestr(hdr, "\r\nCookie:"); int id; char tk[40], ex[40];
    if (!c || !(c = strstr(c, "sid="))) return 0;
    if (sscanf(c, "sid=%d.%39[0-9a-f]", &id, tk) != 2 || !load_user(id, 0, 0, u)) return 0;
    token(u, ex); return !strcmp(ex, tk);
}

/* ---------- funds & revenue ---------- */
static int load_fund(int uid, Fund *f, int max) {
    char p[64], ln[300]; int n = 0; FILE *fp; sprintf(p, "data/fund_%d.txt", uid); fp = fopen(p, "r"); if (!fp) return 0;
    while (n < max && fgets(ln, sizeof ln, fp)) {
        ln[strcspn(ln, "\r\n")] = 0;
        if (sscanf(ln, "%d|%7[^|]|%d|%lf|%63[^\n]", &f[n].id, f[n].month, &f[n].type, &f[n].amt, f[n].note) == 5) n++;
    }
    fclose(fp); return n;
}
static void save_fund(int uid, Fund *f, int n) {
    char p[64]; FILE *fp; int i; sprintf(p, "data/fund_%d.txt", uid); fp = fopen(p, "w"); if (!fp) return;
    for (i = 0; i < n; i++) fprintf(fp, "%d|%s|%d|%.2f|%s\n", f[i].id, f[i].month, f[i].type, f[i].amt, f[i].note);
    fclose(fp);
}

/* ---------- branding, employees, time tracking ---------- */
typedef struct { int n, target; char name[64], post[40], join[8], lid[32]; double sal; unsigned long ph; } Emp;
typedef struct { double hrs; int days, tasks, open; long since; } Stat;
static char g_acc[8] = "#a8802f", g_pri[8] = "#12203a", g_tab[64], g_tag[80], g_brand[64];
static char g_slug[48], g_caddr[160], g_cphone[40], g_cmail[96];
static int g_uid, g_font, g_nfont, g_ncase, g_nsize, g_nbold, g_nital, g_mode, g_rad, g_hdr;

static int okcol(const char *c) {
    int i; if (strlen(c) != 7 || c[0] != '#') return 0;
    for (i = 1; i < 7; i++) if (!isxdigit((unsigned char)c[i])) return 0;
    return 1;
}
static int dark_col(const char *c) {          /* 1 if the colour is dark, so it needs white text */
    int r, g, b; if (sscanf(c + 1, "%2x%2x%2x", &r, &g, &b) != 3) return 1;
    return (r * 299 + g * 587 + b * 114) / 1000 < 150;
}
static void rdline(FILE *f, char *b, size_t n) {
    char t[256];
    if (!fgets(t, sizeof t, f)) { b[0] = 0; return; }
    t[strcspn(t, "\r\n")] = 0;
    strncpy(b, t, n - 1); b[n - 1] = 0;
}
static int rdint(FILE *f, int hi, int def) {
    char t[32]; int v; rdline(f, t, sizeof t);
    if (!t[0]) return def;
    v = atoi(t); return (v < 0 || v > hi) ? def : v;
}
static void apply_prof(int uid) {
    char p[64], a[8] = "", b[8] = ""; FILE *f; User t;
    strcpy(g_acc, "#a8802f"); strcpy(g_pri, "#12203a");
    g_tab[0] = g_tag[0] = g_brand[0] = g_slug[0] = g_caddr[0] = g_cphone[0] = g_cmail[0] = 0; g_uid = uid;
    g_font = 0; g_nfont = 2; g_ncase = 0; g_nsize = 1; g_nbold = 1; g_nital = 0; g_mode = 0; g_rad = 1; g_hdr = 0;
    if (uid && load_user(uid, 0, 0, &t)) { strcpy(g_brand, t.name); strcpy(g_slug, t.slug); }
    sprintf(p, "data/prof_%d.txt", uid);
    if (uid && (f = fopen(p, "r"))) {
        rdline(f, a, sizeof a); rdline(f, b, sizeof b); rdline(f, g_tab, sizeof g_tab); rdline(f, g_tag, sizeof g_tag);
        if (okcol(a)) strcpy(g_acc, a);
        if (okcol(b)) strcpy(g_pri, b);
        fclose(f);
    }
    if (uid) {
        sprintf(p, "data/style_%d.txt", uid);
        if ((f = fopen(p, "r"))) {
            g_font = rdint(f, 5, 0); g_nfont = rdint(f, 5, 2); g_ncase = rdint(f, 3, 0); g_nsize = rdint(f, 3, 1);
            g_nbold = rdint(f, 1, 1); g_nital = rdint(f, 1, 0); g_mode = rdint(f, 2, 0); g_rad = rdint(f, 2, 1); g_hdr = rdint(f, 2, 0);
            fclose(f);
        }
        sprintf(p, "data/contact_%d.txt", uid);
        if ((f = fopen(p, "r"))) {
            rdline(f, g_caddr, sizeof g_caddr); rdline(f, g_cphone, sizeof g_cphone); rdline(f, g_cmail, sizeof g_cmail);
            fclose(f);
        }
    }
}
static void month_now(char *m) { time_t t = time(0); strftime(m, 8, "%Y-%m", localtime(&t)); }
/* emp_<uid>.txt line: n|name|post|salary|target|hash|join|loginid   ("-" = no custom login ID) */
static int load_emp(int uid, Emp *e, int max) {
    char p[64], ln[360]; int n = 0, k; FILE *f; sprintf(p, "data/emp_%d.txt", uid); f = fopen(p, "r"); if (!f) return 0;
    while (n < max && fgets(ln, sizeof ln, f)) {
        e[n].lid[0] = 0;
        k = sscanf(ln, "%d|%63[^|]|%39[^|]|%lf|%d|%lu|%7[^|]|%31[^|\r\n]", &e[n].n, e[n].name, e[n].post, &e[n].sal,
                   &e[n].target, &e[n].ph, e[n].join, e[n].lid);
        if (k == 6) strcpy(e[n].join, "0000-00");
        if (k < 8 || e[n].lid[0] == '-') e[n].lid[0] = 0;
        if (k >= 6) n++;
    }
    fclose(f); return n;
}
static void save_emp(int uid, Emp *e, int n) {
    char p[64]; int i; FILE *f; sprintf(p, "data/emp_%d.txt", uid); f = fopen(p, "w"); if (!f) return;
    for (i = 0; i < n; i++) fprintf(f, "%d|%s|%s|%.2f|%d|%lu|%s|%s\n", e[i].n, e[i].name, e[i].post, e[i].sal, e[i].target,
                                    e[i].ph, e[i].join, e[i].lid[0] ? e[i].lid : "-");
    fclose(f);
}
/* Find an employee by custom login ID. cid > 0: only that company; cid == 0: every company. */
static int find_custom(const char *lid, int cid, int *ru, int *rn) {
    Emp *es = malloc(sizeof(Emp) * 200); FILE *f = fopen("data/users.txt", "r"); char ln[400]; int id, ne, i, found = 0;
    if (!f) { free(es); return 0; }
    while (!found && fgets(ln, sizeof ln, f)) {
        if (sscanf(ln, "%d", &id) != 1 || (cid > 0 && id != cid)) continue;
        ne = load_emp(id, es, 200);
        for (i = 0; i < ne; i++) if (es[i].lid[0] && lideq(es[i].lid, lid)) { *ru = id; *rn = es[i].n; found = 1; break; }
    }
    fclose(f); free(es); return found;
}

/* ---------- goals (owner -> one employee) ---------- */
typedef struct { int id, emp, done; char due[12], text[200]; } Goal;       /* goal_<uid>.txt: id|emp|done|due|text */
static int load_goal(int uid, Goal *g, int max) {
    char p[64], ln[400]; int n = 0; FILE *f; sprintf(p, "data/goal_%d.txt", uid); f = fopen(p, "r"); if (!f) return 0;
    while (n < max && fgets(ln, sizeof ln, f)) {
        ln[strcspn(ln, "\r\n")] = 0;
        if (sscanf(ln, "%d|%d|%d|%11[^|]|%199[^\n]", &g[n].id, &g[n].emp, &g[n].done, g[n].due, g[n].text) == 5) n++;
    }
    fclose(f); return n;
}
static void save_goal(int uid, Goal *g, int n) {
    char p[64]; FILE *f; int i; sprintf(p, "data/goal_%d.txt", uid); f = fopen(p, "w"); if (!f) return;
    for (i = 0; i < n; i++) fprintf(f, "%d|%d|%d|%s|%s\n", g[i].id, g[i].emp, g[i].done, g[i].due, g[i].text);
    fclose(f);
}

/* ---------- meetings (who = 0 for all employees, or one employee number) ---------- */
typedef struct { int id, who, dur; long ts; char title[80], place[100]; } Meet; /* meet_<uid>.txt: id|who|epoch|minutes|title|place */
static int mcmp(const void *a, const void *b) {
    long x = ((const Meet *)a)->ts, y = ((const Meet *)b)->ts; return x < y ? -1 : x > y;
}
static int load_meet(int uid, Meet *m, int max) {
    char p[64], ln[400]; int n = 0; FILE *f; sprintf(p, "data/meet_%d.txt", uid); f = fopen(p, "r"); if (!f) return 0;
    while (n < max && fgets(ln, sizeof ln, f)) {
        ln[strcspn(ln, "\r\n")] = 0;
        if (sscanf(ln, "%d|%d|%ld|%d|%79[^|]|%99[^\n]", &m[n].id, &m[n].who, &m[n].ts, &m[n].dur, m[n].title, m[n].place) == 6) n++;
    }
    fclose(f); qsort(m, n, sizeof(Meet), mcmp); return n;
}
static void save_meet(int uid, Meet *m, int n) {
    char p[64]; FILE *f; int i; sprintf(p, "data/meet_%d.txt", uid); f = fopen(p, "w"); if (!f) return;
    for (i = 0; i < n; i++) fprintf(f, "%d|%d|%ld|%d|%s|%s\n", m[i].id, m[i].who, m[i].ts, m[i].dur, m[i].title, m[i].place);
    fclose(f);
}

/* payroll for month mo: salaries of employees who joined on or before that month */
static double payroll(const Emp *e, int n, const char *mo, int *cnt) {
    double s = 0; int i; *cnt = 0;
    for (i = 0; i < n; i++) if (strcmp(e[i].join, mo) <= 0) { s += e[i].sal; (*cnt)++; }
    return s;
}
static void etoken(int uid, const Emp *e, char *t) { char b[200]; sprintf(b, "E%d-%d%lu%s", uid, e->n, e->ph, SECRET); sprintf(t, "%lx", hs(b)); }
static int eauth(const char *hdr, Emp *out_e, int *cu) {
    const char *c = strcasestr(hdr, "\r\nCookie:"); int uid, n, i, ne; char tk[40], ex[40]; Emp *es;
    if (!c || !(c = strstr(c, "emp="))) return 0;
    if (sscanf(c, "emp=%d.%d.%39[0-9a-f]", &uid, &n, tk) != 3) return 0;
    es = malloc(sizeof(Emp) * 200); ne = load_emp(uid, es, 200);
    for (i = 0; i < ne; i++) if (es[i].n == n) {
        etoken(uid, &es[i], ex);
        if (!strcmp(ex, tk)) { *out_e = es[i]; *cu = uid; free(es); return 1; }
    }
    free(es); return 0;
}
static void logev(int uid, int n, char c) {
    char p[64]; FILE *f; sprintf(p, "data/log_%d.txt", uid); f = fopen(p, "a");
    if (f) { fprintf(f, "%d|%c|%ld\n", n, c, (long)time(0)); fclose(f); }
}
static void mon(long t, char *m) { time_t x = t; strftime(m, 8, "%Y-%m", localtime(&x)); }
/* hours, days present and tasks done by employee n in month mo (clock in/out/task log) */
static void stats(int uid, int n, const char *mo, Stat *s) {
    char p[64], ln[100], a[8], b[12], lastd[12] = "", ty; FILE *f; int id; long t, in = 0; time_t x;
    memset(s, 0, sizeof *s); sprintf(p, "data/log_%d.txt", uid); f = fopen(p, "r"); if (!f) return;
    while (fgets(ln, sizeof ln, f)) {
        if (sscanf(ln, "%d|%c|%ld", &id, &ty, &t) != 3 || id != n) continue;
        mon(t, a);
        if (ty == 'I') {
            in = t; s->open = 1;
            if (!strcmp(a, mo)) { x = t; strftime(b, sizeof b, "%Y-%m-%d", localtime(&x)); if (strcmp(b, lastd)) { s->days++; strcpy(lastd, b); } }
        } else if (ty == 'O' && s->open) {
            mon(in, a); if (!strcmp(a, mo)) s->hrs += (t - in) / 3600.0; s->open = 0;
        } else if (ty == 'T' && !strcmp(a, mo)) s->tasks++;
    }
    fclose(f);
    if (s->open) { s->since = in; mon(in, a); if (!strcmp(a, mo)) s->hrs += (time(0) - in) / 3600.0; }
}
static double eff(const Emp *e, const Stat *s) { return s->days ? s->tasks * 100.0 / (e->target * s->days) : 0; }

/* ---------- http ---------- */
static void sendall(int fd, const char *b, size_t n) {
    while (n) { int w = send(fd, b, (int)n, 0); if (w <= 0) return; b += w; n -= w; }
}
static void reply(int fd, const char *st, const char *ct, const char *body, size_t n, const char *extra) {
    char h[600];
    int l = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                     "Connection: close\r\n%s\r\n", st, ct, (unsigned long)n, extra ? extra : "");
    sendall(fd, h, l); sendall(fd, body, n);
}
static void html(int fd) { reply(fd, "200 OK", "text/html; charset=utf-8", out, ol, 0); }
static void redirect(int fd, const char *to, const char *cookie) {
    char x[300]; snprintf(x, sizeof x, "Location: %s\r\n%s", to, cookie ? cookie : "");
    reply(fd, "303 See Other", "text/plain", "", 0, x);
}

/* ---------- pages ---------- */
/* Colours come from CSS variables, which head() fills in from the company's theme. */
static const char *CSS =
"<style>*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--txt);font-size:16px;line-height:1.6}"
"h1,h2,h3,.logo{font-weight:600;letter-spacing:.01em}"
"a{color:var(--brass)}header{background:var(--hbg);color:var(--hfg);padding:18px 6vw;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px;border-bottom:3px solid var(--brass)}"
"header a{color:var(--hfg);opacity:.85;text-decoration:none;margin-left:22px}header a:hover{opacity:1;text-decoration:underline}"
".logo{font-size:26px;color:var(--hfg);margin:0}"
"main{max-width:1000px;margin:0 auto;padding:36px 6vw}.hero{display:grid;grid-template-columns:1.2fr 1fr;gap:48px;align-items:center;min-height:70vh}"
".hero h1{font-size:clamp(36px,5vw,60px);line-height:1.1;margin:0 0 16px}.hero p{color:var(--mut);max-width:44ch}"
".panel{background:var(--panel);border:1px solid var(--line);border-radius:var(--rad);padding:28px;box-shadow:0 18px 40px -26px rgba(0,0,0,.45);overflow-x:auto}"
"label{display:block;font-size:14px;color:var(--mut);margin:12px 0 4px}"
"input,select{width:100%;padding:10px 12px;border:1px solid var(--line);border-radius:var(--rad);background:var(--inp);font:inherit;color:var(--txt)}"
"input[type=color]{padding:2px;height:42px}"
"input:focus,select:focus,button:focus-visible{outline:2px solid var(--brass);outline-offset:1px}"
"button,.btn{background:var(--brass);color:var(--bfg);border:0;border-radius:var(--brad);padding:11px 22px;font:inherit;cursor:pointer;margin-top:18px;text-decoration:none;display:inline-block}"
"button:hover,.btn:hover{filter:brightness(1.12)}.ghost{background:none;color:var(--mut);padding:4px 8px;margin:0;border:1px solid var(--line)}"
".sws{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px;margin-top:8px}"
".sw{margin:0;padding:8px 10px;border:1px solid var(--line);background:var(--inp);color:var(--txt);display:flex;align-items:center;gap:8px;font-size:13px}"
".sw:hover{filter:none;border-color:var(--brass)}.sw i{width:34px;height:20px;border-radius:4px;display:inline-block}"
".row{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:14px}table{width:100%;border-collapse:collapse;margin-top:8px}"
"th{text-align:left;font-weight:600;color:var(--mut);font-size:14px}th,td{padding:10px 8px;border-bottom:1px solid var(--line);vertical-align:middle}"
"td img{width:52px;height:52px;object-fit:cover;border:1px solid var(--line);border-radius:var(--rad)}.num{text-align:right}.tot td{font-weight:700;border-top:2px solid var(--txt);border-bottom:0}"
".kpi{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:14px}.kpi div{border:1px solid var(--line);border-radius:var(--rad);padding:14px;background:var(--inp)}"
".kpi b{display:block;font-size:22px}.kpi span{color:var(--mut);font-size:13px}.pos{color:#1d6b3a}.neg{color:#8b1e1e}"
".err{color:#b03030;margin:0 0 8px}.stack>*+*{margin-top:28px}footer{text-align:center;color:var(--mut);font-size:13px;padding:30px}"
"@media(max-width:760px){.hero{grid-template-columns:1fr}}"
"@media print{header,.noprint,footer{display:none}body,.panel{background:#fff!important;color:#000!important}.panel{box-shadow:none;border:0}}</style>";

/* "https://host" of the address the browser used (works on Render, ngrok, localhost, your own domain) */
static void ap_base(void) {
    int local = g_host[0] && (strstr(g_host, "localhost") || strstr(g_host, "127.0.0.1"));
    if (!g_host[0]) { ap(PUBLIC_SCHEME "://" PUBLIC_HOST); return; }
    ap("%s://", g_proto[0] ? g_proto : (local ? "http" : "https")); esc(g_host);
}
static void ap_url(const char *slug) { ap_base(); ap("/w/"); esc(slug); }

/* in: 0 plain, 1 owner tabs, 2 employee tabs, 3 home tabs, 4 company/public tabs */
static void head(const char *title, int in) {
    const char *bg = "#eef0f2", *pn = "#ffffff", *tx = "#12203a", *ln = "#d5d9df", *mu = "#5b667a", *ip = "#fbfbfc";
    static const char *rad[] = { "0px", "10px", "18px" };
    static const char *brad[] = { "0px", "8px", "999px" };
    const char *hfg, *bfg; char hbg[140];
    if (g_mode == 1) { bg = "#0f141c"; pn = "#18202c"; tx = "#e8ecf2"; ln = "#2c3646"; mu = "#9aa6ba"; ip = "#101722"; }
    else if (g_mode == 2) { bg = "#f6f1e7"; pn = "#fffdf8"; tx = "#2b2418"; ln = "#e3d9c6"; mu = "#7a6e58"; ip = "#fbf7ee"; }
    if (g_hdr == 2) { snprintf(hbg, sizeof hbg, "var(--panel)"); hfg = tx; }
    else {
        if (g_hdr == 1) snprintf(hbg, sizeof hbg, "linear-gradient(120deg,%s,%s)", g_pri, g_acc);
        else snprintf(hbg, sizeof hbg, "%s", g_pri);
        hfg = dark_col(g_pri) ? "#ffffff" : "#111111";
    }
    bfg = dark_col(g_acc) ? "#ffffff" : "#111111";
    ap("<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>");
    esc(title); ap(" | "); esc(g_tab[0] ? g_tab : (g_brand[0] ? g_brand : APPNAME));
    ap("</title>");
    if (g_uid) ap("<link rel=icon href='/logo?u=%d'>", g_uid);
    ap("%s<style>:root{--brass:%s;--ink:%s;--bg:%s;--panel:%s;--txt:%s;--line:%s;--mut:%s;--inp:%s;--rad:%s;--brad:%s;--hfg:%s;--bfg:%s;--hbg:%s}"
       "body{font-family:%s}h1,h2,h3{font-family:%s}"
       ".logo{font-family:%s;text-transform:%s;font-size:%s;font-weight:%d;font-style:%s}%s</style></head><body><header><p class=logo>",
       CSS, g_acc, g_pri, bg, pn, tx, ln, mu, ip, rad[g_rad], brad[g_rad], hfg, bfg, hbg,
       FONTS[g_font], FONTS[g_font],
       FONTS[g_nfont], CASES[g_ncase], SIZES[g_nsize], g_nbold ? 700 : 400, g_nital ? "italic" : "normal",
       g_mode == 1 ? ".pos{color:#5fd48a}.neg,.err{color:#ff8a8a}" : "");
    if (g_uid) ap("<img src='/logo?u=%d' alt='' onerror='this.remove()' style='height:34px;vertical-align:middle;margin-right:12px'>", g_uid);
    esc(g_brand[0] ? g_brand : APPNAME);
    if (g_tag[0]) { ap("<small style='font:13px sans-serif;font-style:normal;text-transform:none;letter-spacing:0;opacity:.8;margin-left:14px'>"); esc(g_tag); ap("</small>"); }
    ap("</p>");
    if (in == 1) ap("<nav><a href=/app>Workspace</a><a href=/team>Team</a><a href=/goals>Goals</a><a href=/meetings>Meetings</a><a href=/track>Work tracking</a><a href=/finance>Growth &amp; funds</a><a href=/bill>Monthly bill</a><a href=/contact>Contact us</a><a href=/settings>Settings</a><a href=/logout>Sign out</a></nav>");
    if (in == 3) ap("<style>html{scroll-behavior:smooth}</style><nav><a href=#register>Register company</a><a href=#owner>Owner sign in</a><a href=#employees>Employee sign in</a></nav>");
    if (in == 2) ap("<nav><a href=/me>My work</a><a href=/contact>Contact us</a><a href=/elogout>Sign out</a></nav>");
    if (in == 4) {
        ap("<nav>");
        if (g_slug[0]) ap("<a href='/w/%s'>Employee sign-in</a>", g_slug);
        if (g_uid) ap("<a href='/contact?c=%d'>Contact us</a>", g_uid);
        ap("<a href=/owner>Owner sign-in</a></nav>");
    }
    ap("</header><main>");
    if (in == 2)   /* meeting reminders: asks the server every 30 s for meetings starting within 15 minutes */
        ap("<div id=mal></div><script>(function(){var b=document.getElementById('mal');"
           "function go(){fetch('/meetalerts').then(function(r){return r.text()}).then(function(t){b.textContent='';"
           "t.split('\\n').forEach(function(l){var p=l.split('|');if(p.length<5)return;"
           "var s=+p[2],m=Math.max(1,Math.ceil(s/60)),d=document.createElement('div');d.className='panel';"
           "d.style.cssText='border-left:6px solid var(--brass);margin-bottom:18px';"
           "d.textContent='Meeting reminder: '+p[3]+' starts in about '+m+' min'+(p[4]!=='-'?' ('+p[4]+')':'');b.appendChild(d);"
           "var k='ma'+p[0]+'_'+p[1];try{if(!localStorage.getItem(k)){localStorage.setItem(k,1);"
           "if(window.Notification&&Notification.permission==='granted')new Notification('Meeting in '+m+' min',{body:p[3]});"
           "alert('Reminder: meeting '+p[3]+' starts in about '+m+' minutes.')}}catch(e){}})}).catch(function(){})}"
           "if(window.Notification&&Notification.permission==='default')Notification.requestPermission();"
           "go();setInterval(go,30000)})()</script>");
}
static void foot(void) {
    ap("</main><script>document.querySelectorAll('[data-t]').forEach(function(e){e.textContent=new Date(e.getAttribute('data-t')*1000)"
       ".toLocaleString([],{dateStyle:'medium',timeStyle:'short'})})</script><footer>");
    if (g_uid && (g_cphone[0] || g_cmail[0])) {
        ap("<div>Contact: ");
        if (g_cphone[0]) esc(g_cphone);
        if (g_cphone[0] && g_cmail[0]) ap(" &middot; ");
        if (g_cmail[0]) esc(g_cmail);
        ap("</div>");
    }
    ap("&copy; 2026 "); esc(g_brand[0] ? g_brand : APPNAME); ap(". Powered by " APPNAME ". All rights reserved by PRANJAL MISHRA.</footer></body></html>");
}

static void page_home(int fd, const char *err) {
    apply_prof(0); ol = 0; head("Welcome", 3);
    ap("<section class=hero><div><h1>Your company, in sync.</h1>"
       "<p>Register your company to get its own " APPNAME " address, your logo, colours and fonts, an employee login for every team member with work-time and efficiency tracking, goals, meetings, and a live view of growth and funds.</p></div>"
       "<div class=stack><div class=panel id=register><h2>Register your company</h2>");
    if (err) { ap("<p class=err>"); esc(err); ap("</p>"); }
    ap("<form id=reg method=post action=/register><label>Company name</label><input name=name required maxlength=60>"
       "<label>Web address name (optional)</label><input name=addr maxlength=40 placeholder='startupmanager'>"
       "<p style='color:var(--mut);font-size:13px;margin:4px 0 0'>Your address: ");
    ap_base();
    ap("/w/<b>name</b>-" INITIALS ". Leave blank to use your company name.</p>"
       "<label>Email</label><input name=email type=email required><label>Password</label><input name=pass type=password minlength=6 required>"
       "<label>Company logo (optional)</label><input id=lg type=file accept='image/*'>"
       "<label>Browser tab title (optional)</label><input name=tab maxlength=60 placeholder='e.g. Acme Portal'>"
       "<div class=row><div><label>Primary colour</label><input name=pri type=color value='#12203a'></div>"
       "<div><label>Accent colour</label><input name=acc type=color value='#a8802f'></div></div>"
       "<button>Create workspace</button></form></div>"
       "<div class=panel id=owner><h3>Already registered?</h3><form method=post action=/login><label>Email</label><input name=email type=email required>"
       "<label>Password</label><input name=pass type=password required><button>Sign in</button></form></div></div></section>"
       "<section id=employees style='margin-top:40px'><div class=panel style='max-width:520px;margin:0 auto'>"
       "<h2>Employee sign-in</h2><p style='color:var(--mut)'>For team members. Use the Login ID and password given by your company "
       "(the Login ID is either a custom ID such as priya@acme, or the default companyID-employeeID such as 3-1). If you open your company's own address, your Employee ID alone (for example 1) is enough.</p>"
       "<form method=post action=/elogin><label>Login ID</label><input name=lid placeholder='e.g. priya@acme or 3-1' required>"
       "<label>Password</label><input name=pass type=password required><button>Employee sign in</button></form></div></section>"
       "<script>document.getElementById('reg').onsubmit=async function(e){e.preventDefault();"
       "var r=await fetch('/register',{method:'POST',body:new URLSearchParams(new FormData(this))});"
       "if(r.url.slice(-4)!=='/app'){document.open();document.write(await r.text());document.close();return}"
       "var f=document.getElementById('lg').files[0];if(f)await fetch('/logo',{method:'POST',body:f});location='/app'}</script>");
    foot(); html(fd);
}
/* company's own branded employee sign-in page: /w/<slug> */
static void page_company(int fd, User *c, const char *err) {
    apply_prof(c->id); ol = 0; head("Employee sign-in", 4);
    ap("<div class=panel style='max-width:440px;margin:40px auto;text-align:center'>"
       "<img src='/logo?u=%d' alt='' onerror='this.remove()' style='max-height:90px;max-width:100%%;margin-bottom:10px'>", c->id);
    ap("<h2>Welcome to "); esc(c->name); ap("</h2><p style='color:var(--mut)'>Employee sign-in</p>");
    if (err) { ap("<p class=err>"); esc(err); ap("</p>"); }
    ap("<form method=post action=/elogin style='text-align:left'><input type=hidden name=co value=%d>"
       "<label>Login ID (custom ID, or Employee ID such as 1)</label><input name=lid placeholder='e.g. priya@acme or 1' required>"
       "<label>Password</label><input name=pass type=password required><button>Sign in</button></form></div>", c->id);
    foot(); html(fd);
}
static void page_contact(int fd, User *c, int mode) {
    apply_prof(c->id); ol = 0; head("Contact us", mode);
    ap("<div class=panel style='max-width:560px;margin:30px auto'><h1>Contact us</h1><p style='color:var(--mut)'>Get in touch with "); esc(c->name); ap(".</p>");
    if (!g_caddr[0] && !g_cphone[0] && !g_cmail[0]) ap("<p>Contact details have not been added yet.</p>");
    else {
        ap("<table>");
        if (g_caddr[0]) { ap("<tr><th>Address<td>"); esc(g_caddr); ap("</tr>"); }
        if (g_cphone[0]) { ap("<tr><th>Phone<td><a href='tel:"); esc(g_cphone); ap("'>"); esc(g_cphone); ap("</a></tr>"); }
        if (g_cmail[0]) { ap("<tr><th>Email<td><a href='mailto:"); esc(g_cmail); ap("'>"); esc(g_cmail); ap("</a></tr>"); }
        ap("</table>");
    }
    if (mode == 1) ap("<p style='color:var(--mut);font-size:14px'>You can change these details in Settings.</p>");
    ap("</div>"); foot(); html(fd);
}

static void page_app(int fd, User *u) {
    Res r[500]; int n = load_res(u->id, r, 500), i; char m[8]; month_now(m);
    ol = 0; head("Workspace", 1);
    ap("<div class=stack><div><h1>"); esc(u->name); ap("</h1><p style='color:var(--mut)'>Your resources and their quantities.</p></div>"
       "<div class=panel><h2>Add a resource</h2><form id=f><div class=row><div><label>Name</label><input name=name required maxlength=60></div>"
       "<div><label>Category</label><select name=cat><option value=1>Equipment<option value=2>Software<option value=3>Space<option value=4>People</select></div>"
       "<div><label>Quantity</label><input name=qty type=number min=0 value=1 required></div>"
       "<div><label>Unit cost (Rs.)</label><input name=cost type=number min=0 step=0.01 required></div>"
       "<div><label>Month</label><input name=month type=month value=%s required></div>"
       "<div><label>Photo</label><input id=ph type=file accept='image/*'></div></div><button>Save resource</button></form></div>"
       "<div class=panel><h2>All resources</h2>", m);
    if (!n) ap("<p>No resources yet. Add your first one above.</p>");
    else {
        ap("<table><tr><th>Photo<th>Name<th>Category<th>Month<th class=num>Qty<th class=num>Unit<th></tr>");
        for (i = 0; i < n; i++) {
            ap("<tr><td>");
            if (r[i].photo) ap("<img src='/photo?rid=%d' alt=''>", r[i].id);
            ap("<td>"); esc(r[i].name);
            ap("<td>%s<td>%s<td class=num>%d<td class=num>%.2f<td><form method=post action=/delete?rid=%d><button class=ghost>Remove</button></form></tr>",
               CAT[r[i].cat], r[i].month, r[i].qty, r[i].cost, r[i].id);
        }
        ap("</table>");
    }
    ap("</div></div>"
       "<script>document.getElementById('f').onsubmit=async function(e){e.preventDefault();var d=new URLSearchParams(new FormData(this));"
       "var r=await fetch('/add',{method:'POST',body:d});var id=(await r.text()).trim();var f=document.getElementById('ph').files[0];"
       "if(f&&/^\\d+$/.test(id))await fetch('/photo?rid='+id,{method:'POST',body:f});location.reload()}</script>");
    foot(); html(fd);
}
static void page_bill(int fd, User *u, const char *m) {
    Res r[500]; int n = load_res(u->id, r, 500), i, c; double sub[5] = {0}, tot = 0, line, pr = 0;
    ol = 0; head("Monthly bill", 1);
    ap("<div class=panel><h1>Monthly expense bill</h1><p>"); esc(u->name);
    ap("</p><form method=get action=/bill class=noprint><label>Month</label><input name=m type=month value=%s style='max-width:200px'>"
       "<button>Show</button> <button type=button onclick='print()'>Print or save PDF</button></form>"
       "<table><tr><th>Item<th>Category<th class=num>Qty<th class=num>Unit (Rs.)<th class=num>Amount (Rs.)</tr>", m);
    for (i = 0; i < n; i++) {
        if (strcmp(r[i].month, m)) continue;
        line = r[i].qty * r[i].cost; sub[r[i].cat] += line; tot += line;
        ap("<tr><td>"); esc(r[i].name);
        ap("<td>%s<td class=num>%d<td class=num>%.2f<td class=num>%.2f</tr>", CAT[r[i].cat], r[i].qty, r[i].cost, line);
    }
    { Emp e[200]; int ne = load_emp(u->id, e, 200), cnt;
      pr = payroll(e, ne, m, &cnt);
      if (cnt) { ap("<tr><td>Employee salaries<td>Payroll<td class=num>%d<td class=num>-<td class=num>%.2f</tr>", cnt, pr); tot += pr; } }
    ap("</table><table style='max-width:340px;margin-left:auto'>");
    for (c = 1; c <= 4; c++) ap("<tr><td>%s subtotal<td class=num>%.2f</tr>", CAT[c], sub[c]);
    ap("<tr><td>Payroll subtotal<td class=num>%.2f</tr>", pr);
    ap("<tr class=tot><td>Total due<td class=num>Rs. %.2f</tr></table></div>", tot);
    foot(); html(fd);
}

/* ---------- growth & funds ---------- */
static void nextmon(char *m) {
    int y = 0, mo = 0; sscanf(m, "%d-%d", &y, &mo);
    if (++mo > 12) { mo = 1; y++; }
    sprintf(m, "%04d-%02d", y, mo);
}
static void page_finance(int fd, User *u) {
    Fund *f = malloc(sizeof(Fund) * 500); Res *r = malloc(sizeof(Res) * 500); Emp *e = malloc(sizeof(Emp) * 200);
    int nf = load_fund(u->id, f, 500), nr = load_res(u->id, r, 500), ne = load_emp(u->id, e, 200), i, hc = 0, guard = 0;
    char first[8] = "9999-99", cur[8], mo[8], g[16];
    double cum = 0, totrev = 0, totfund = 0, prevrev = -1, lastrev = 0, lastexp = 0, rev, fnd, rx, pay, ex, net;
    month_now(cur);
    for (i = 0; i < nf; i++) if (strcmp(f[i].month, first) < 0) strcpy(first, f[i].month);
    for (i = 0; i < nr; i++) if (strcmp(r[i].month, first) < 0) strcpy(first, r[i].month);
    for (i = 0; i < ne; i++) if (strcmp(e[i].join, "0000-00") && strcmp(e[i].join, first) < 0) strcpy(first, e[i].join);
    ol = 0; head("Growth & funds", 1);
    ap("<div class=stack><div><h1>Company growth &amp; funds</h1><p style='color:var(--mut)'>Record money received and revenue earned. Expenses come automatically from your resources and payroll.</p></div>"
       "<div class=panel><h2>Add funds or revenue</h2><form method=post action=/fundadd><div class=row>"
       "<div><label>Type</label><select name=type><option value=1>Funding received<option value=2>Revenue earned</select></div>"
       "<div><label>Month</label><input name=month type=month value=%s required></div>"
       "<div><label>Amount (Rs.)</label><input name=amount type=number min=0.01 step=0.01 required></div>"
       "<div><label>Note</label><input name=note maxlength=60 placeholder='e.g. Seed round'></div></div><button>Save</button></form></div>", cur);
    if (first[0] == '9') ap("<div class=panel><p>No data yet. Add funding, revenue, resources or employees to see growth.</p></div>");
    else {
        ap("<div class=panel><h2>Month by month</h2><table><tr><th>Month<th class=num>Funding<th class=num>Revenue<th class=num>Revenue growth"
           "<th class=num>Expenses<th class=num>Net<th class=num>Cash balance<th class=num>Staff</tr>");
        strcpy(mo, first);
        while (strcmp(mo, cur) <= 0 && guard++ < 120) {
            rev = fnd = rx = 0;
            for (i = 0; i < nf; i++) if (!strcmp(f[i].month, mo)) { if (f[i].type == 2) rev += f[i].amt; else fnd += f[i].amt; }
            for (i = 0; i < nr; i++) if (!strcmp(r[i].month, mo)) rx += r[i].qty * r[i].cost;
            pay = payroll(e, ne, mo, &hc); ex = rx + pay; net = rev + fnd - ex; cum += net;
            totrev += rev; totfund += fnd;
            if (prevrev > 0) snprintf(g, sizeof g, "%+.0f%%", (rev - prevrev) * 100.0 / prevrev); else strcpy(g, "-");
            ap("<tr><td>%s<td class=num>%.2f<td class=num>%.2f<td class=num>%s<td class=num>%.2f<td class='num %s'>%.2f<td class=num>%.2f<td class=num>%d</tr>",
               mo, fnd, rev, g, ex, net >= 0 ? "pos" : "neg", net, cum, hc);
            prevrev = rev; lastrev = rev; lastexp = ex; nextmon(mo);
        }
        ap("</table></div><div class=panel><h2>Where you stand</h2><div class=kpi>"
           "<div><b>Rs. %.2f</b><span>Funds available (cash balance)</span></div>"
           "<div><b>Rs. %.2f</b><span>Total funding raised</span></div>"
           "<div><b>Rs. %.2f</b><span>Total revenue</span></div>"
           "<div><b>%d</b><span>Employees this month</span></div>", cum, totfund, totrev, hc);
        if (lastexp - lastrev > 0 && cum > 0) ap("<div><b>%.1f months</b><span>Runway at this month's net burn</span></div>", cum / (lastexp - lastrev));
        else if (lastexp - lastrev <= 0) ap("<div><b>Self-sustaining</b><span>Revenue covers expenses this month</span></div>");
        else ap("<div><b>Out of funds</b><span>Balance is not positive</span></div>");
        ap("</div></div>");
    }
    if (nf) {
        ap("<div class=panel><h2>Entries</h2><table><tr><th>Month<th>Type<th>Note<th class=num>Amount<th></tr>");
        for (i = 0; i < nf; i++) {
            ap("<tr><td>%s<td>%s<td>", f[i].month, f[i].type == 2 ? "Revenue" : "Funding"); esc(f[i].note);
            ap("<td class=num>%.2f<td><form method=post action=/funddel?rid=%d><button class=ghost>Remove</button></form></tr>", f[i].amt, f[i].id);
        }
        ap("</table></div>");
    }
    ap("</div>"); foot(); html(fd); free(f); free(r); free(e);
}

/* ---------- employee pages ---------- */
static void page_elogin(int fd, const char *err) {
    apply_prof(0); ol = 0; head("Employee sign-in", 4);
    ap("<div class=panel style='max-width:420px;margin:40px auto'><h2>Employee sign-in</h2>");
    if (err) { ap("<p class=err>"); esc(err); ap("</p>"); }
    ap("<form method=post action=/elogin><label>Login ID (given by your company)</label><input name=lid placeholder='e.g. priya@acme or 3-1' required>"
       "<label>Password</label><input name=pass type=password required><button>Sign in</button></form></div>");
    foot(); html(fd);
}

/* ---------- goals page (owner) ---------- */
static void page_goals(int fd, User *u) {
    Emp *e = malloc(sizeof(Emp) * 200); Goal *g = malloc(sizeof(Goal) * 500);
    int ne = load_emp(u->id, e, 200), ng = load_goal(u->id, g, 500), i, j;
    ol = 0; head("Goals", 1);
    ap("<div class=stack><div><h1>Goals</h1><p style='color:var(--mut)'>Set a goal for each employee separately. It appears on that employee's own screen, and they can mark it done.</p></div>");
    if (!ne) ap("<div class=panel><p>Add employees on the Team page first.</p></div>");
    else {
        ap("<div class=panel><h2>Set a goal</h2><form method=post action=/goaladd><div class=row><div><label>Employee</label><select name=emp>");
        for (i = 0; i < ne; i++) { ap("<option value=%d>", e[i].n); esc(e[i].name); ap(" - "); esc(e[i].post); }
        ap("</select></div><div><label>Deadline (optional)</label><input name=due type=date></div></div>"
           "<label>Goal</label><input name=text required maxlength=190 placeholder='e.g. Finish the client dashboard by Friday'>"
           "<button>Assign goal</button></form></div>");
    }
    ap("<div class=panel><h2>All goals</h2>");
    if (!ng) ap("<p>No goals set yet.</p>");
    else {
        ap("<table><tr><th>Employee<th>Goal<th>Deadline<th>Status<th></tr>");
        for (i = 0; i < ng; i++) {
            for (j = 0; j < ne; j++) if (e[j].n == g[i].emp) break;
            ap("<tr><td>"); esc(j < ne ? e[j].name : "(removed)"); ap("<td>"); esc(g[i].text);
            ap("<td>"); esc(g[i].due); ap("<td>%s<td><form method=post action=/goaldel?rid=%d><button class=ghost>Remove</button></form></tr>",
                                          g[i].done ? "Done" : "Open", g[i].id);
        }
        ap("</table>");
    }
    ap("</div></div>"); foot(); html(fd); free(e); free(g);
}

/* ---------- meetings page (owner) ---------- */
static void page_meetings(int fd, User *u) {
    Emp *e = malloc(sizeof(Emp) * 200); Meet *mt = malloc(sizeof(Meet) * 300);
    int ne = load_emp(u->id, e, 200), nm = load_meet(u->id, mt, 300), i, j; long now = (long)time(0);
    ol = 0; head("Meetings", 1);
    ap("<div class=stack><div><h1>Meetings</h1><p style='color:var(--mut)'>Schedule a meeting for everyone or for one employee. "
       "Employees get an alert 15 minutes before it starts (while they have their employee page open). Times are shown in each person's own time zone.</p></div>"
       "<div class=panel><h2>Schedule a meeting</h2>"
       "<form method=post action=/meetadd onsubmit=\"this.ts.value=Math.floor(new Date(this.when.value).getTime()/1000)\">"
       "<input type=hidden name=ts><div class=row>"
       "<div><label>Title</label><input name=title required maxlength=78></div>"
       "<div><label>Date and time</label><input name=when type=datetime-local required></div>"
       "<div><label>Length (minutes)</label><input name=dur type=number min=5 max=600 value=30 required></div>"
       "<div><label>Who should attend</label><select name=who><option value=0>All employees");
    for (j = 0; j < ne; j++) { ap("<option value=%d>", e[j].n); esc(e[j].name); }
    ap("</select></div><div><label>Place or meeting link</label><input name=place maxlength=95></div></div>"
       "<button>Schedule meeting</button></form></div><div class=panel><h2>All meetings</h2>");
    if (!nm) ap("<p>No meetings scheduled.</p>");
    else {
        ap("<table><tr><th>When<th>Title<th>Length<th>Attendees<th>Place<th>Status<th></tr>");
        for (i = 0; i < nm; i++) {
            ap("<tr><td><span data-t=%ld></span><td>", mt[i].ts); esc(mt[i].title);
            ap("<td>%d min<td>", mt[i].dur);
            if (!mt[i].who) ap("Everyone");
            else { for (j = 0; j < ne; j++) if (e[j].n == mt[i].who) break; esc(j < ne ? e[j].name : "(removed)"); }
            ap("<td>"); esc(strcmp(mt[i].place, "-") ? mt[i].place : "");
            ap("<td>%s<td><form method=post action=/meetdel?rid=%d><button class=ghost>Remove</button></form></tr>",
               mt[i].ts + mt[i].dur * 60L < now ? "Finished" : mt[i].ts <= now ? "Happening now" : "Upcoming", mt[i].id);
        }
        ap("</table>");
    }
    ap("</div></div>"); foot(); html(fd); free(e); free(mt);
}

/* goals and meetings shown on an employee's own screen */
static void emp_extras(int cu, const Emp *e) {
    Goal *g = malloc(sizeof(Goal) * 500); Meet *mt = malloc(sizeof(Meet) * 300);
    int ng = load_goal(cu, g, 500), nm = load_meet(cu, mt, 300), i, c = 0; long now = (long)time(0);
    ap("<div class=panel><h2>My goals</h2>");
    for (i = 0; i < ng; i++) if (g[i].emp == e->n) {
        if (!c++) ap("<table><tr><th>Goal<th>Deadline<th>Status<th></tr>");
        ap("<tr><td>"); esc(g[i].text); ap("<td>"); esc(g[i].due);
        ap("<td>%s<td>", g[i].done ? "Done" : "Open");
        if (!g[i].done) ap("<form method=post action=/goaldone?rid=%d><button class=ghost>Mark done</button></form>", g[i].id);
        ap("</tr>");
    }
    if (c) ap("</table>"); else ap("<p>No goals have been set for you yet.</p>");
    ap("</div><div class=panel><h2>Upcoming meetings</h2>"); c = 0;
    for (i = 0; i < nm; i++) if ((mt[i].who == 0 || mt[i].who == e->n) && mt[i].ts + mt[i].dur * 60L >= now) {
        if (!c++) ap("<table><tr><th>When<th>Meeting<th>Length<th>Place</tr>");
        ap("<tr><td><span data-t=%ld></span><td>", mt[i].ts); esc(mt[i].title);
        ap("<td>%d min<td>", mt[i].dur); esc(strcmp(mt[i].place, "-") ? mt[i].place : ""); ap("</tr>");
    }
    if (c) ap("</table>"); else ap("<p>No upcoming meetings.</p>");
    ap("</div>"); free(g); free(mt);
}
static void page_me(int fd, Emp *e, int cu) {
    Stat st; char m[8], t[12]; time_t x;
    apply_prof(cu); month_now(m); stats(cu, e->n, m, &st);
    ol = 0; head("My work", 2);
    ap("<div class=stack><div><h1>"); esc(e->name); ap("</h1><p style='color:var(--mut)'>"); esc(e->post);
    ap(", employee ID %d</p></div><div class=panel><h2>%s</h2>", e->n, st.open ? "You are clocked in" : "You are clocked out");
    if (st.open) { x = st.since; strftime(t, sizeof t, "%H:%M", localtime(&x)); ap("<p>Working since %s.</p>", t); }
    ap("<form method=post action=/clock><button>%s</button></form>", st.open ? "Clock out" : "Clock in");
    if (st.open) ap("<form method=post action=/task><button>Log a completed task</button></form>");
    ap("</div>");
    emp_extras(cu, e);
    ap("<div class=panel><h2>This month</h2><table><tr><th>Hours worked<th>Days present<th>Tasks done<th>Efficiency</tr>"
       "<tr><td>%.1f<td>%d<td>%d<td>%.0f%% of target (%d tasks a day)</tr></table></div></div>", st.hrs, st.days, st.tasks, eff(e, &st), e->target);
    foot(); html(fd);
}
static void page_team(int fd, User *u, int ec) {
    Emp e[200]; int n = load_emp(u->id, e, 200), i, j, np = 0, pc[200]; char po[200][40]; double pay = 0;
    ol = 0; head("Team", 1);
    ap("<div class=stack><div><h1>Team</h1></div>");
    if (ec) ap("<p class=err>%s</p>", ec == 1 ? "Login ID must be 3-30 characters: letters, numbers and @ only (no spaces or other symbols), with at least one letter."
                                  : ec == 2 ? "That Login ID is already taken. Choose a different one." : "Could not save that Login ID.");
    ap("<div class=panel><h2>Add an employee</h2><form method=post action=/empadd><div class=row>"
       "<div><label>Name</label><input name=name required maxlength=60></div>"
       "<div><label>Post</label><input name=post required maxlength=38 placeholder='e.g. Developer'></div>"
       "<div><label>Monthly salary (Rs.)</label><input name=salary type=number min=0 step=0.01 required></div>"
       "<div><label>Target tasks per day</label><input name=target type=number min=1 value=5 required></div>"
       "<div><label>Password for the employee</label><input name=pass minlength=6 required></div>"
       "<div><label>Custom Login ID (optional)</label><input name=lid maxlength=30 pattern='[A-Za-z0-9@]{3,30}' "
       "title='Letters, numbers and @ only, at least one letter' placeholder='e.g. priya@acme'></div></div>"
       "<button>Add employee</button></form><p style='color:var(--mut)'>Each employee can sign in with the default Login ID (shown in the table below, like 3-1) "
       "or with a custom Login ID you choose: letters, numbers and @ only. Give them the Login ID and the password you set. Your company address for employees is in Settings.</p></div><div class=panel><h2>Employees</h2>");
    for (i = 0; i < n; i++) {
        pay += e[i].sal;
        for (j = 0; j < np; j++) if (!strcmp(po[j], e[i].post)) break;
        if (j == np) { strcpy(po[np], e[i].post); pc[np++] = 0; }
        pc[j]++;
    }
    if (!n) ap("<p>No employees yet.</p>");
    else {
        ap("<p>Headcount: %d. Monthly payroll: Rs. %.2f.<br>", n, pay);
        for (j = 0; j < np; j++) { esc(po[j]); ap(": %d%s", pc[j], j + 1 < np ? ", " : ""); }
        ap("</p><table><tr><th>Employee ID<th>Login ID<th>Name<th>Post<th>Joined<th class=num>Salary<th></tr>");
        for (i = 0; i < n; i++) {
            ap("<tr><td>%d<td><b>%d-%d</b>", e[i].n, u->id, e[i].n);
            if (e[i].lid[0]) { ap("<br><b>"); esc(e[i].lid); ap("</b>"); }
            ap("<form method=post action=/empsetid?rid=%d><input name=lid maxlength=30 placeholder='custom ID' value='", e[i].n); esc(e[i].lid);
            ap("' style='max-width:150px;padding:4px 8px;margin-top:4px'> <button class=ghost>Set</button></form><td>"); esc(e[i].name);
            ap("<td>"); esc(e[i].post);
            ap("<td>%s<td class=num>%.2f<td><form method=post action=/empdel?rid=%d><button class=ghost>Remove</button></form></tr>",
               strcmp(e[i].join, "0000-00") ? e[i].join : "-", e[i].sal, e[i].n);
        }
        ap("</table>");
    }
    ap("</div></div>"); foot(); html(fd);
}
static void page_track(int fd, User *u, const char *m) {
    Emp e[200]; int n = load_emp(u->id, e, 200), i; Stat st;
    ol = 0; head("Work tracking", 1);
    ap("<div class=panel><h1>Work time and efficiency</h1><form method=get action=/track><label>Month</label>"
       "<input name=m type=month value=%s style='max-width:200px'><button>Show</button></form>"
       "<p style='color:var(--mut)'>Efficiency is tasks completed against the daily target for the days present.</p>", m);
    if (!n) ap("<p>Add employees on the Team page first.</p>");
    else {
        ap("<table><tr><th>Employee<th>Post<th class=num>Days<th class=num>Hours<th class=num>Tasks<th class=num>Efficiency<th>Now</tr>");
        for (i = 0; i < n; i++) {
            stats(u->id, e[i].n, m, &st); ap("<tr><td>"); esc(e[i].name); ap("<td>"); esc(e[i].post);
            ap("<td class=num>%d<td class=num>%.1f<td class=num>%d<td class=num>%.0f%%<td>%s</tr>", st.days, st.hrs, st.tasks, eff(&e[i], &st), st.open ? "Working" : "Off");
        }
        ap("</table>");
    }
    ap("</div>"); foot(); html(fd);
}

/* ---------- settings ---------- */
static void opt(int v, const char *label, int sel) { ap("<option value=%d%s>%s", v, sel ? " selected" : "", label); }
static void fopt(int v, int sel) { ap("<option value=%d data-f=\"%s\"%s>%s", v, FONTS[v], sel ? " selected" : "", FNAMES[v]); }
static void page_settings(int fd, User *u) {
    int i;
    ol = 0; head("Settings", 1);
    ap("<div class=stack><h1>Brand and workspace</h1><form id=sf class=stack onchange='pv()'>"
       "<div class=panel><h2>Company address</h2><p style='color:var(--mut)'>Share this address with your employees so they can sign in. Select it to copy.</p>"
       "<input readonly onclick='this.select()' value='");
    ap_url(u->slug);
    ap("'></div>"
       "<div class=panel><h2>Identity</h2><div class=row><div><label>Browser tab title</label><input name=tab maxlength=60 value='"); esc(g_tab);
    ap("'></div><div><label>Tagline</label><input name=tag maxlength=78 value='"); esc(g_tag);
    ap("'></div><div><label>New logo (optional)</label><input id=lg type=file accept='image/*'></div></div></div>"
       "<div class=panel><h2>Colour theme</h2><p style='color:var(--mut)'>Pick a ready-made palette, then fine-tune the colours below.</p><div class=sws>");
    for (i = 0; i < (int)(sizeof PRE / sizeof PRE[0]); i++)
        ap("<button type=button class=sw onclick=\"pick('%s','%s',%d)\"><i style='background:linear-gradient(135deg,%s 50%%,%s 50%%)'></i>%s</button>",
           PRE[i].pri, PRE[i].acc, PRE[i].mode, PRE[i].pri, PRE[i].acc, PRE[i].name);
    ap("</div><div class=row style='margin-top:10px'><div><label>Primary colour</label><input name=pri type=color value=%s></div>"
       "<div><label>Accent colour</label><input name=acc type=color value=%s></div>"
       "<div><label>Page mode</label><select name=mode>", g_pri, g_acc);
    for (i = 0; i < 3; i++) opt(i, MODEN[i], g_mode == i);
    ap("</select></div><div><label>Corners</label><select name=rad>");
    for (i = 0; i < 3; i++) opt(i, RADN[i], g_rad == i);
    ap("</select></div><div><label>Header style</label><select name=hdr>");
    for (i = 0; i < 3; i++) opt(i, HDRN[i], g_hdr == i);
    ap("</select></div></div></div>"
       "<div class=panel><h2>Fonts and company name</h2><div class=row><div><label>Site font</label><select name=font>");
    for (i = 0; i < 6; i++) fopt(i, g_font == i);
    ap("</select></div><div><label>Company name font</label><select name=nfont>");
    for (i = 0; i < 6; i++) fopt(i, g_nfont == i);
    ap("</select></div><div><label>Company name letters</label><select name=ncase>");
    for (i = 0; i < 4; i++) opt(i, CASEN[i], g_ncase == i);
    ap("</select></div><div><label>Company name size</label><select name=nsize>");
    for (i = 0; i < 4; i++) opt(i, SIZEN[i], g_nsize == i);
    ap("</select></div><div><label>Company name weight</label><select name=nbold>");
    opt(1, "Bold", g_nbold == 1); opt(0, "Light", g_nbold == 0);
    ap("</select></div><div><label>Company name slant</label><select name=nital>");
    opt(0, "Upright", g_nital == 0); opt(1, "Italic", g_nital == 1);
    ap("</select></div></div><p style='margin-top:16px;color:var(--mut)'>Preview: <span id=pv>"); esc(g_brand[0] ? g_brand : APPNAME);
    ap("</span></p></div>"
       "<div class=panel><h2>Contact us details</h2><p style='color:var(--mut)'>Shown on your Contact us page to employees and visitors.</p><div class=row>"
       "<div><label>Company address</label><input name=caddr maxlength=150 value='"); esc(g_caddr);
    ap("'></div><div><label>Phone number</label><input name=cphone type=tel maxlength=38 value='"); esc(g_cphone);
    ap("'></div><div><label>Company email</label><input name=cmail type=email maxlength=90 value='"); esc(g_cmail);
    ap("'></div></div></div><div><button>Save settings</button></div></form></div>"
       "<script>function pick(p,a,m){var d=document.getElementById('sf').elements;d['pri'].value=p;d['acc'].value=a;d['mode'].value=m}"
       "function pv(){var d=document.getElementById('sf').elements,s=document.getElementById('pv'),f=d['nfont'],"
       "c=['none','uppercase','lowercase','capitalize'],z=['20px','26px','34px','44px'];"
       "s.style.fontFamily=f.options[f.selectedIndex].getAttribute('data-f');s.style.textTransform=c[d['ncase'].value];"
       "s.style.fontSize=z[d['nsize'].value];s.style.fontWeight=d['nbold'].value==1?700:400;s.style.fontStyle=d['nital'].value==1?'italic':'normal'}"
       "pv();document.getElementById('sf').onsubmit=async function(e){e.preventDefault();"
       "await fetch('/settings',{method:'POST',body:new URLSearchParams(new FormData(this))});"
       "var f=document.getElementById('lg').files[0];if(f)await fetch('/logo',{method:'POST',body:f});location.reload()}</script>");
    foot(); html(fd);
}
static void serve_img(int fd, const char *p) {
    char *d; long sz; FILE *f = fopen(p, "rb"); const char *ct = "image/jpeg";
    if (!f) { reply(fd, "404 Not Found", "text/plain", "", 0, 0); return; }
    fseek(f, 0, SEEK_END); sz = ftell(f); rewind(f); d = malloc(sz + 1); if (fread(d, 1, sz, f) != (size_t)sz) sz = 0; fclose(f);
    if (sz > 4 && (unsigned char)d[0] == 0x89) ct = "image/png"; else if (sz > 4 && d[0] == 'G') ct = "image/gif";
    else if (sz > 12 && !memcmp(d, "RIFF", 4)) ct = "image/webp";
    reply(fd, "200 OK", ct, d, sz, "X-Content-Type-Options: nosniff\r\n"); free(d);
}

/* ---------- request handler ---------- */
static void handle(int fd) {
    char *buf = malloc(MAXBODY + 16384); int len = 0, hl = 0, cl = 0; char *body, *q, m[8], path[128], meth[8];
    User u; int logged;
#ifdef _WIN32
    DWORD tv = 10000;                  /* milliseconds on Windows */
#else
    struct timeval tv = { 10, 0 };
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    while (len < MAXBODY) {
        int r = recv(fd, buf + len, (int)(MAXBODY + 16000 - len), 0); if (r <= 0) break;
        len += r; buf[len] = 0;
        if (!hl && (body = strstr(buf, "\r\n\r\n"))) {
            hl = body - buf + 4; q = strcasestr(buf, "\r\nContent-Length:"); cl = q ? atoi(q + 17) : 0;
            if (cl > MAXBODY) { reply(fd, "413 Too Large", "text/plain", "Too large", 9, 0); goto done; }
        }
        if (hl && len >= hl + cl) break;
    }
    if (!hl || sscanf(buf, "%7s %127s", meth, path) != 2) goto done;
    body = buf + hl; body[len - hl] = 0;
    g_host[0] = 0; g_proto[0] = 0;
    if ((q = strcasestr(buf, "\r\nHost:"))) {
        size_t hn; q += 7; while (*q == ' ') q++;
        hn = strcspn(q, "\r\n"); if (hn >= sizeof g_host) hn = sizeof g_host - 1;
        memcpy(g_host, q, hn); g_host[hn] = 0;
    }
    if ((q = strcasestr(buf, "\r\nX-Forwarded-Proto:"))) {
        q += 20; while (*q == ' ') q++;
        strcpy(g_proto, !strncasecmp(q, "https", 5) ? "https" : "http");
    }
    q = strchr(path, '?'); if (q) *q++ = 0; else q = path + strlen(path);
    logged = auth(buf, &u); month_now(m); if (logged) apply_prof(u.id);
    char v[300], mm[8], ck[200], tk[40]; int rid = 0;
    if (!param(q, "m", mm, sizeof mm) || strlen(mm) != 7) strcpy(mm, m);
    if (param(q, "rid", v, sizeof v)) rid = atoi(v);

    if (!strcmp(path, "/") && !strcmp(meth, "GET")) {
        char sl[48]; User cu; char *d = strchr(g_host, '.'); sl[0] = 0;
        /* company sub-domain: acme.<PUBLIC_HOST> or acme.localhost */
        if (d && (!strncasecmp(d + 1, PUBLIC_HOST, strlen(PUBLIC_HOST)) || !strncasecmp(d + 1, "localhost", 9))) {
            size_t l = d - g_host; if (l < sizeof sl) { memcpy(sl, g_host, l); sl[l] = 0; }
        }
        if (sl[0] && load_user(0, 0, sl, &cu)) page_company(fd, &cu, 0);
        else if (logged) redirect(fd, "/app", 0);
        else page_home(fd, 0);
    }
    else if (!strcmp(path, "/owner") && !strcmp(meth, "GET")) { if (logged) redirect(fd, "/app", 0); else page_home(fd, 0); }
    else if (!strncmp(path, "/w/", 3)) {
        char sl[48]; User cu; size_t l = strcspn(path + 3, "/");
        if (l && l < sizeof sl) { memcpy(sl, path + 3, l); sl[l] = 0; } else sl[0] = 0;
        if (sl[0] && load_user(0, 0, sl, &cu)) page_company(fd, &cu, 0);
        else reply(fd, "404 Not Found", "text/plain", "No such company", 15, 0);
    }
    else if (!strcmp(path, "/contact") && !strcmp(meth, "GET")) {
        /* works for the owner, a signed-in employee, or the public (?c=<company id>) */
        Emp ce; User cc; int ecu = 0, cid = 0, mode = 4;
        if (param(q, "c", v, sizeof v)) cid = atoi(v);
        if (logged && (cid == 0 || cid == u.id)) { cid = u.id; mode = 1; }
        else if (eauth(buf, &ce, &ecu) && (cid == 0 || cid == ecu)) { cid = ecu; mode = 2; }
        if (cid > 0 && load_user(cid, 0, 0, &cc)) page_contact(fd, &cc, mode);
        else reply(fd, "404 Not Found", "text/plain", "No such company", 15, 0);
    }
    else if (!strcmp(path, "/register") && !strcmp(meth, "POST")) {
        char nm[64], em[96], pw[64], sal[200], a[8], p2[8], t1[64], pf[64]; User t;
        param(body, "name", nm, sizeof nm); param(body, "email", em, sizeof em); param(body, "pass", pw, sizeof pw);
        if (!*nm || !*em || strlen(pw) < 6) page_home(fd, "Fill every field; password needs 6+ characters.");
        else if (load_user(0, em, 0, &t)) page_home(fd, "That email is already registered.");
        else {
            MKDIR("data"); MKDIR("data/photos");
            memset(&t, 0, sizeof t);
            t.id = next_user_id(); snprintf(sal, sizeof sal, "%s%s", em, pw); t.ph = hs(sal);
            strncpy(t.name, nm, sizeof t.name - 1); strncpy(t.email, em, sizeof t.email - 1);
            { char ad[48]; size_t sl2; param(body, "addr", ad, sizeof ad); slugify(*ad ? ad : nm, t.slug, 36);
              sl2 = strlen(t.slug); if (sl2 < 3 || strcmp(t.slug + sl2 - 3, "-" INITIALS)) strcat(t.slug, "-" INITIALS); }
            { User x; if (load_user(0, 0, t.slug, &x)) { char s2[48]; snprintf(s2, sizeof s2, "%s-%d", t.slug, t.id); strcpy(t.slug, s2); } }
            FILE *f = fopen("data/users.txt", "a"); fprintf(f, "%d|%s|%s|%lu|%s\n", t.id, t.name, t.email, t.ph, t.slug); fclose(f);
            param(body, "acc", a, sizeof a); param(body, "pri", p2, sizeof p2); param(body, "tab", t1, sizeof t1);
            if (!okcol(a)) strcpy(a, "#a8802f");
            if (!okcol(p2)) strcpy(p2, "#12203a");
            sprintf(pf, "data/prof_%d.txt", t.id); f = fopen(pf, "w");
            if (f) { fprintf(f, "%s\n%s\n%s\n\n", a, p2, t1); fclose(f); }
            token(&t, tk); snprintf(ck, sizeof ck, "Set-Cookie: sid=%d.%s; Path=/; HttpOnly; SameSite=Lax\r\n", t.id, tk);
            redirect(fd, "/app", ck);
        }
    } else if (!strcmp(path, "/login") && !strcmp(meth, "POST")) {
        char em[96], pw[64], sal[200]; User t;
        param(body, "email", em, sizeof em); param(body, "pass", pw, sizeof pw); snprintf(sal, sizeof sal, "%s%s", em, pw);
        if (load_user(0, em, 0, &t) && t.ph == hs(sal)) {
            token(&t, tk); snprintf(ck, sizeof ck, "Set-Cookie: sid=%d.%s; Path=/; HttpOnly; SameSite=Lax\r\n", t.id, tk);
            redirect(fd, "/app", ck);
        } else page_home(fd, "Email or password is incorrect.");
    } else if (!strcmp(path, "/logout")) redirect(fd, "/", "Set-Cookie: sid=; Path=/; Max-Age=0\r\n");
    else if (!strcmp(path, "/logo") && !strcmp(meth, "GET")) {
        char p[64]; param(q, "u", v, sizeof v); sprintf(p, "data/logo_%d.img", atoi(v)); serve_img(fd, p);
    }
    else if (!strcmp(path, "/elogin") && !strcmp(meth, "GET")) page_elogin(fd, 0);
    else if (!strcmp(path, "/elogin") && !strcmp(meth, "POST")) {
        /* Accepts a custom Login ID (letters/digits/@), "companyID-employeeID" (e.g. 3-1) anywhere,
           or just the employee number when the form came from the company's own page (hidden field co). */
        char lid[40], pw[64], sal[120], co[16]; Emp *es = malloc(sizeof(Emp) * 200);
        int eu = 0, en = 0, ne, k, ok = 0, cid, parsed = 0; User cu;
        param(body, "lid", lid, sizeof lid); param(body, "pass", pw, sizeof pw); param(body, "co", co, sizeof co);
        trim(lid);
        cid = atoi(co);
        if (strchr(lid, '-')) parsed = (sscanf(lid, "%d-%d", &eu, &en) == 2);
        else if (valid_lid(lid)) parsed = find_custom(lid, cid, &eu, &en);
        else if (cid > 0 && isdigit((unsigned char)lid[0])) { eu = cid; en = atoi(lid); parsed = 1; }
        if (parsed && cid > 0 && eu != cid) parsed = 0;               /* company page: ID must belong to that company */
        if (parsed && eu > 0 && en > 0) {
            ne = load_emp(eu, es, 200); snprintf(sal, sizeof sal, "%d-%d%s", eu, en, pw);
            for (k = 0; k < ne && !ok; k++) if (es[k].n == en && es[k].ph == hs(sal)) {
                etoken(eu, &es[k], tk);
                snprintf(ck, sizeof ck, "Set-Cookie: emp=%d.%d.%s; Path=/; HttpOnly; SameSite=Lax\r\n", eu, en, tk);
                redirect(fd, "/me", ck); ok = 1;
            }
        }
        free(es);
        if (!ok) {
            if (cid > 0 && load_user(cid, 0, 0, &cu)) page_company(fd, &cu, "Login ID or password is incorrect.");
            else page_elogin(fd, "Login ID or password is incorrect. Use the Login ID given by your company, like priya@acme or 3-1.");
        }
    }
    else if (!strcmp(path, "/elogout")) redirect(fd, "/elogin", "Set-Cookie: emp=; Path=/; Max-Age=0\r\n");
    else if (!strcmp(path, "/me") || !strcmp(path, "/clock") || !strcmp(path, "/task") ||
             !strcmp(path, "/goaldone") || !strcmp(path, "/meetalerts")) {
        Emp e; int cu = 0; Stat st;
        if (!eauth(buf, &e, &cu)) redirect(fd, "/elogin", 0);
        else {
            stats(cu, e.n, m, &st);
            if (!strcmp(path, "/clock") && !strcmp(meth, "POST")) { logev(cu, e.n, st.open ? 'O' : 'I'); redirect(fd, "/me", 0); }
            else if (!strcmp(path, "/task") && !strcmp(meth, "POST")) { if (st.open) logev(cu, e.n, 'T'); redirect(fd, "/me", 0); }
            else if (!strcmp(path, "/goaldone") && !strcmp(meth, "POST")) {
                Goal *g = malloc(sizeof(Goal) * 500); int ng = load_goal(cu, g, 500), i;
                for (i = 0; i < ng; i++) if (g[i].id == rid && g[i].emp == e.n) g[i].done = 1;
                save_goal(cu, g, ng); free(g); redirect(fd, "/me", 0);
            }
            else if (!strcmp(path, "/meetalerts")) {
                /* one line per meeting starting within 15 minutes: id|start|seconds left|title|place */
                Meet *mt = malloc(sizeof(Meet) * 300); int nm = load_meet(cu, mt, 300), i; long now = (long)time(0), d;
                ol = 0;
                for (i = 0; i < nm; i++) {
                    d = mt[i].ts - now;
                    if ((mt[i].who == 0 || mt[i].who == e.n) && d > 0 && d <= ALERT_SECS)
                        ap("%d|%ld|%ld|%s|%s\n", mt[i].id, mt[i].ts, d, mt[i].title, mt[i].place);
                }
                reply(fd, "200 OK", "text/plain; charset=utf-8", out, ol, "Cache-Control: no-store\r\n"); free(mt);
            }
            else page_me(fd, &e, cu);
        }
    }
    else if (!logged) redirect(fd, "/", 0);
    else if (!strcmp(path, "/app")) page_app(fd, &u);
    else if (!strcmp(path, "/bill")) page_bill(fd, &u, mm);
    else if (!strcmp(path, "/team")) page_team(fd, &u, param(q, "e", v, sizeof v) ? atoi(v) : 0);
    else if (!strcmp(path, "/track")) page_track(fd, &u, mm);
    else if (!strcmp(path, "/finance")) page_finance(fd, &u);
    else if (!strcmp(path, "/goals")) page_goals(fd, &u);
    else if (!strcmp(path, "/meetings")) page_meetings(fd, &u);
    else if (!strcmp(path, "/goaladd") && !strcmp(meth, "POST")) {
        Goal *g = malloc(sizeof(Goal) * 501), *x; Emp *es = malloc(sizeof(Emp) * 200);
        int n = load_goal(u.id, g, 500), ne = load_emp(u.id, es, 200), i, mx = 0, ok = 0;
        for (i = 0; i < n; i++) if (g[i].id > mx) mx = g[i].id;
        x = &g[n]; memset(x, 0, sizeof *x); x->id = mx + 1;
        param(body, "emp", v, sizeof v); x->emp = atoi(v);
        for (i = 0; i < ne; i++) if (es[i].n == x->emp) ok = 1;
        param(body, "due", x->due, sizeof x->due); if (!okdate(x->due)) strcpy(x->due, "-");
        param(body, "text", x->text, sizeof x->text); trim(x->text);
        if (ok && n < 500 && x->text[0]) save_goal(u.id, g, n + 1);
        free(g); free(es); redirect(fd, "/goals", 0);
    }
    else if (!strcmp(path, "/goaldel") && !strcmp(meth, "POST")) {
        Goal *g = malloc(sizeof(Goal) * 500); int n = load_goal(u.id, g, 500), i, k = 0;
        for (i = 0; i < n; i++) if (g[i].id != rid) g[k++] = g[i];
        save_goal(u.id, g, k); free(g); redirect(fd, "/goals", 0);
    }
    else if (!strcmp(path, "/meetadd") && !strcmp(meth, "POST")) {
        Meet *mt = malloc(sizeof(Meet) * 301), *x; Emp *es = malloc(sizeof(Emp) * 200);
        int n = load_meet(u.id, mt, 300), ne = load_emp(u.id, es, 200), i, mx = 0, ok = 0;
        for (i = 0; i < n; i++) if (mt[i].id > mx) mx = mt[i].id;
        x = &mt[n]; memset(x, 0, sizeof *x); x->id = mx + 1;
        param(body, "title", x->title, sizeof x->title); trim(x->title);
        param(body, "ts", v, sizeof v); x->ts = atol(v);
        param(body, "dur", v, sizeof v); x->dur = atoi(v); if (x->dur < 5 || x->dur > 600) x->dur = 30;
        param(body, "who", v, sizeof v); x->who = atoi(v);
        if (x->who == 0) ok = 1; else for (i = 0; i < ne; i++) if (es[i].n == x->who) ok = 1;
        param(body, "place", x->place, sizeof x->place); trim(x->place); if (!x->place[0]) strcpy(x->place, "-");
        if (ok && n < 300 && x->title[0] && x->ts > 0) save_meet(u.id, mt, n + 1);
        free(mt); free(es); redirect(fd, "/meetings", 0);
    }
    else if (!strcmp(path, "/meetdel") && !strcmp(meth, "POST")) {
        Meet *mt = malloc(sizeof(Meet) * 300); int n = load_meet(u.id, mt, 300), i, k = 0;
        for (i = 0; i < n; i++) if (mt[i].id != rid) mt[k++] = mt[i];
        save_meet(u.id, mt, k); free(mt); redirect(fd, "/meetings", 0);
    }
    else if (!strcmp(path, "/empsetid") && !strcmp(meth, "POST")) {
        Emp *es = malloc(sizeof(Emp) * 200); int n = load_emp(u.id, es, 200), i, idx = -1, eu, en, err = 0; char li[40];
        param(body, "lid", li, sizeof li); trim(li);
        for (i = 0; i < n; i++) if (es[i].n == rid) idx = i;
        if (idx < 0) err = 3;
        else if (!li[0]) es[idx].lid[0] = 0;                              /* empty = remove the custom ID */
        else if (!valid_lid(li)) err = 1;
        else if (find_custom(li, 0, &eu, &en) && !(eu == u.id && en == rid)) err = 2;
        else strcpy(es[idx].lid, li);
        if (!err) save_emp(u.id, es, n);
        free(es); redirect(fd, err == 1 ? "/team?e=1" : err == 2 ? "/team?e=2" : err ? "/team?e=3" : "/team", 0);
    }
    else if (!strcmp(path, "/fundadd") && !strcmp(meth, "POST")) {
        Fund *f = malloc(sizeof(Fund) * 501), *x; int n = load_fund(u.id, f, 500), i, mx = 0;
        for (i = 0; i < n; i++) if (f[i].id > mx) mx = f[i].id;
        x = &f[n]; memset(x, 0, sizeof *x); x->id = mx + 1;
        param(body, "type", v, sizeof v); x->type = atoi(v) == 2 ? 2 : 1;
        param(body, "month", x->month, sizeof x->month); if (strlen(x->month) != 7) strcpy(x->month, m);
        param(body, "amount", v, sizeof v); x->amt = atof(v);
        param(body, "note", x->note, sizeof x->note); if (!*x->note) strcpy(x->note, "-");
        if (n < 500 && x->amt > 0) save_fund(u.id, f, n + 1);
        free(f); redirect(fd, "/finance", 0);
    }
    else if (!strcmp(path, "/funddel") && !strcmp(meth, "POST")) {
        Fund *f = malloc(sizeof(Fund) * 500); int n = load_fund(u.id, f, 500), i, k = 0;
        for (i = 0; i < n; i++) if (f[i].id != rid) f[k++] = f[i];
        save_fund(u.id, f, k); free(f); redirect(fd, "/finance", 0);
    }
    else if (!strcmp(path, "/settings") && !strcmp(meth, "GET")) page_settings(fd, &u);
    else if (!strcmp(path, "/settings") && !strcmp(meth, "POST")) {
        char a[8], b2[8], t1[64], t2[80], p[64], z[16], ca[160], cp[40], cm[96]; FILE *f;
        static const char *keys[] = { "font", "nfont", "ncase", "nsize", "nbold", "nital", "mode", "rad", "hdr" };
        static const int hi[] = { 5, 5, 3, 3, 1, 1, 2, 2, 2 };
        int k;
        param(body, "acc", a, sizeof a); param(body, "pri", b2, sizeof b2); param(body, "tab", t1, sizeof t1); param(body, "tag", t2, sizeof t2);
        if (!okcol(a)) { strcpy(a, "#a8802f"); }
        if (!okcol(b2)) { strcpy(b2, "#12203a"); }
        sprintf(p, "data/prof_%d.txt", u.id); f = fopen(p, "w");
        if (f) { fprintf(f, "%s\n%s\n%s\n%s\n", a, b2, t1, t2); fclose(f); }
        sprintf(p, "data/style_%d.txt", u.id); f = fopen(p, "w");
        if (f) { for (k = 0; k < 9; k++) { param(body, keys[k], z, sizeof z); fprintf(f, "%d\n", ci(z, hi[k])); } fclose(f); }
        param(body, "caddr", ca, sizeof ca); param(body, "cphone", cp, sizeof cp); param(body, "cmail", cm, sizeof cm);
        sprintf(p, "data/contact_%d.txt", u.id); f = fopen(p, "w");
        if (f) { fprintf(f, "%s\n%s\n%s\n", ca, cp, cm); fclose(f); }
        reply(fd, "200 OK", "text/plain", "ok", 2, 0);
    }
    else if (!strcmp(path, "/logo") && !strcmp(meth, "POST")) {
        char p[64]; FILE *f; sprintf(p, "data/logo_%d.img", u.id); f = fopen(p, "wb");
        if (f) { fwrite(body, 1, len - hl, f); fclose(f); }
        reply(fd, "200 OK", "text/plain", "ok", 2, 0);
    }
    else if (!strcmp(path, "/empadd") && !strcmp(meth, "POST")) {
        Emp *es = malloc(sizeof(Emp) * 201), *x; int n = load_emp(u.id, es, 200), i, mx = 0, eu, en, err = 0; char pw[64], sal[120], li[40];
        for (i = 0; i < n; i++) if (es[i].n > mx) mx = es[i].n;
        x = &es[n]; memset(x, 0, sizeof *x); x->n = mx + 1; strcpy(x->join, m);
        param(body, "name", x->name, sizeof x->name); param(body, "post", x->post, sizeof x->post);
        param(body, "salary", v, sizeof v); x->sal = atof(v); if (x->sal < 0) x->sal = 0;
        param(body, "target", v, sizeof v); x->target = atoi(v); if (x->target < 1) x->target = 1;
        param(body, "pass", pw, sizeof pw);
        param(body, "lid", li, sizeof li); trim(li);                      /* optional custom Login ID */
        if (li[0]) {
            if (!valid_lid(li)) err = 1;
            else if (find_custom(li, 0, &eu, &en)) err = 2;
            else strcpy(x->lid, li);
        }
        if (!err && n < 200 && *x->name && *x->post && strlen(pw) >= 6) {
            snprintf(sal, sizeof sal, "%d-%d%s", u.id, x->n, pw); x->ph = hs(sal); save_emp(u.id, es, n + 1);
        }
        free(es); redirect(fd, err == 1 ? "/team?e=1" : err == 2 ? "/team?e=2" : "/team", 0);
    }
    else if (!strcmp(path, "/empdel") && !strcmp(meth, "POST")) {
        Emp *es = malloc(sizeof(Emp) * 200); int n = load_emp(u.id, es, 200), i, k = 0;
        for (i = 0; i < n; i++) if (es[i].n != rid) es[k++] = es[i];
        save_emp(u.id, es, k); free(es); redirect(fd, "/team", 0);
    }
    else if (!strcmp(path, "/add") && !strcmp(meth, "POST")) {
        Res *r = malloc(sizeof(Res) * 501), *x; int n = load_res(u.id, r, 500), i, mx = 0;
        if (n >= 500) { reply(fd, "400 Bad", "text/plain", "full", 4, 0); free(r); goto done; }
        for (i = 0; i < n; i++) if (r[i].id > mx) mx = r[i].id;
        x = &r[n]; memset(x, 0, sizeof *x); x->id = mx + 1;
        param(body, "name", x->name, sizeof x->name);
        param(body, "cat", v, sizeof v); x->cat = atoi(v); if (x->cat < 1 || x->cat > 4) x->cat = 1;
        param(body, "qty", v, sizeof v); x->qty = atoi(v); if (x->qty < 0) x->qty = 0;
        param(body, "cost", v, sizeof v); x->cost = atof(v); if (x->cost < 0) x->cost = 0;
        param(body, "month", x->month, sizeof x->month); if (strlen(x->month) != 7) strcpy(x->month, m);
        if (!*x->name) { reply(fd, "400 Bad", "text/plain", "name", 4, 0); free(r); goto done; }
        save_res(u.id, r, n + 1); sprintf(v, "%d", x->id); reply(fd, "200 OK", "text/plain", v, strlen(v), 0); free(r);
    } else if (!strcmp(path, "/photo") && !strcmp(meth, "POST")) {
        Res *r = malloc(sizeof(Res) * 500); int n = load_res(u.id, r, 500), i, ok = 0; char p[80];
        for (i = 0; i < n; i++) if (r[i].id == rid) {
            sprintf(p, "data/photos/%d_%d.img", u.id, rid); FILE *f = fopen(p, "wb");
            if (f) { fwrite(body, 1, len - hl, f); fclose(f); r[i].photo = ok = 1; }
        }
        if (ok) { save_res(u.id, r, n); } free(r);
        reply(fd, ok ? "200 OK" : "404 Not Found", "text/plain", "", 0, 0);
    } else if (!strcmp(path, "/photo")) {
        char p[80]; sprintf(p, "data/photos/%d_%d.img", u.id, rid); serve_img(fd, p);
    } else if (!strcmp(path, "/delete") && !strcmp(meth, "POST")) {
        Res *r = malloc(sizeof(Res) * 500); int n = load_res(u.id, r, 500), i, k = 0; char p[80];
        for (i = 0; i < n; i++) if (r[i].id != rid) r[k++] = r[i];
        sprintf(p, "data/photos/%d_%d.img", u.id, rid); remove(p); save_res(u.id, r, k); free(r);
        redirect(fd, "/app", 0);
    } else reply(fd, "404 Not Found", "text/plain", "Not found", 9, 0);
done:
    free(buf);
}

int main(void) {
    int s, on = 1; struct sockaddr_in a;
#ifdef _WIN32
    WSADATA wsa; if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 1;
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    MKDIR("data"); MKDIR("data/photos");
    s = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(PORT);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 16) < 0) { perror("bind"); return 1; }
    printf(APPNAME " listening on port %d (local: http://localhost:%d)\n", PORT, PORT);
    for (;;) { int c = accept(s, 0, 0); if (c < 0) continue; handle(c); CLOSESOCK(c); }
}
