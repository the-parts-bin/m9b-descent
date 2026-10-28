// Descent on the m9b SoC: the few POSIX functions D1X's Linux code uses
// beyond picolibc and ../../core/runtime/syscalls.c (files through FatFs).
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#include "ff.h"
#include "m9b_map.h"

#define CLOCK_CNT (*(volatile uint32_t*)(M9B_TIMING + 0xCu))

// No console input (the UART is the keyboard, see m9b_input.c).
static int no_input(FILE *f) { (void)f; return EOF; }
static FILE input = FDEV_SETUP_STREAM(NULL, no_input, NULL, _FDEV_SETUP_READ);
FILE *const stdin = &input;

int isatty(int fd) { return fd < 3; }

int access(const char *path, int mode) {
    (void)mode;
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK ? 0 : -1;
}

int usleep(useconds_t us) {
    uint32_t t0 = CLOCK_CNT;
    while (CLOCK_CNT - t0 < us * 36u) {
    }
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    static uint32_t last, sec, frac;   // CLOCK_CNT (36 MHz) wraps every 119 s
    uint32_t now = CLOCK_CNT;
    frac += now - last;
    last = now;
    sec += frac / 36000000u;
    frac %= 36000000u;
    tv->tv_sec = sec;
    tv->tv_usec = frac / 36u;
    return 0;
}

// glob(): "dir/pattern" with * and ? in the last part only (what Descent
// asks for: *.plr, *.msn and the like), case-insensitive as on DOS.
static int match(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (const char *t = s;; t++) {
                if (match(p + 1, t)) return 1;
                if (!*t) return 0;
            }
        }
        if (!*s) return 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
    }
    return !*s;
}

int glob(const char *restrict pattern, int flags, int (*errfunc)(const char *, int), glob_t *restrict g) {
    (void)flags; (void)errfunc;
    memset(g, 0, sizeof(*g));
    const char *slash = strrchr(pattern, '/');
    char dir[128] = "";
    const char *pat = pattern;
    if (slash) {
        size_t n = (size_t)(slash - pattern);
        if (n >= sizeof(dir)) return GLOB_NOMATCH;
        memcpy(dir, pattern, n);
        dir[n] = 0;
        pat = slash + 1;
    }
    DIR d;
    if (f_opendir(&d, dir) != FR_OK) return GLOB_NOMATCH;
    int cap = 0;
    FILINFO fi;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if ((fi.fattrib & AM_DIR) || !match(pat, fi.fname)) continue;
        if (g->gl_pathc + 1 >= cap) {
            cap = cap ? 2 * cap : 16;
            char **v = realloc(g->gl_pathv, sizeof(char *) * cap);
            if (!v) break;
            g->gl_pathv = v;
        }
        size_t len = strlen(dir) + 1 + strlen(fi.fname) + 1;
        char *path = malloc(len);
        if (!path) break;
        snprintf(path, len, "%s%s%s", dir, *dir ? "/" : "", fi.fname);
        g->gl_pathv[g->gl_pathc++] = path;
        g->gl_pathv[g->gl_pathc] = NULL;
    }
    f_closedir(&d);
    g->gl_matchc = g->gl_pathc;
    return g->gl_pathc ? 0 : GLOB_NOMATCH;
}

void globfree(glob_t *g) {
    for (int i = 0; i < g->gl_pathc; i++) free(g->gl_pathv[i]);
    free(g->gl_pathv);
    g->gl_pathv = NULL;
    g->gl_pathc = 0;
}
