// Descent on the m9b SoC, measurement builds only (-DM9B_MEMPROF, with
// -Wl,--wrap=memcpy): who calls memcpy, and with how many bytes; printed
// with the profile report (m9b_memprof_report).
#ifdef M9B_MEMPROF
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void *__real_memcpy(void *d, const void *s, size_t n);

static struct { uintptr_t ra; uint32_t calls, bytes; } callers[256];

void m9b_memprof_note(unsigned long ra, unsigned n) {
    for (int i = 0; i < 256; i++) {
        if (callers[i].ra == ra || !callers[i].ra) {
            callers[i].ra = ra;
            callers[i].calls++;
            callers[i].bytes += n;
            break;
        }
    }
}

void *__wrap_memcpy(void *d, const void *s, size_t n) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    for (int i = 0; i < 256; i++) {
        if (callers[i].ra == ra || !callers[i].ra) {
            callers[i].ra = ra;
            callers[i].calls++;
            callers[i].bytes += n;
            break;
        }
    }
    return __real_memcpy(d, s, n);
}

void m9b_memprof_report(void) {
    for (int i = 0; i < 256 && callers[i].ra; i++) {
        if (callers[i].bytes > 50000u)
            printf("memcpy from %08lx: %lu calls, %lu KB\n", (unsigned long)callers[i].ra,
                   (unsigned long)callers[i].calls, (unsigned long)(callers[i].bytes / 1024u));
        callers[i].calls = callers[i].bytes = 0;
    }
}
#endif
