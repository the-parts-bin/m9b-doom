// SD read check (make EXTRA=-DM9B_SDTEST): multi-block reads (CMD18)
// against single-block reads (CMD17) of the same sectors, and the WAD's
// checksum read in large and in small pieces.
#ifdef M9B_SDTEST
#include <fcntl.h>
#include "clock.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sd.h"
#include "m9b_map.h"

#define CLOCK_CNT (*(volatile uint32_t*)(M9B_TIMING + 0xCu))

static uint32_t sum(const uint8_t *p, uint32_t n, uint32_t *a, uint32_t *s) {
    for (uint32_t i = 0; i < n; i++) { *a += p[i]; *s += *a; }
    return *a ^ *s << 16 ^ *s >> 16;
}

static void file_sum(const char *path, uint32_t chunk, uint8_t *buf) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("sdtest: can't open %s\n", path); return; }
    uint32_t a = 1, s = 0, total = 0, r = 0, t0 = CLOCK_CNT;
    int n;
    while ((n = read(fd, buf, chunk)) > 0) { r = sum(buf, (uint32_t)n, &a, &s); total += (uint32_t)n; }
    uint32_t ms = (CLOCK_CNT - t0) / CLOCK_PER_MS;
    close(fd);
    printf("sdtest: %s in %lu-byte reads: %lu bytes, sum %08lx, %lu ms (%s)\n", path, (unsigned long)chunk,
           (unsigned long)total, (unsigned long)r, (unsigned long)ms, n < 0 ? "read error" : "ok");
}

void sdtest(void) {
    uint8_t *m = malloc(65536 + 1), *s1 = malloc(16 * 512);
    uint32_t bad = 0, errs = 0;
    file_sum("DOOMS/DOOM1.WAD", 65536, m);   // (also mounts the card)
    for (uint32_t lba = 0; lba < 8192u; lba += 16u) {
        if (sd_read(lba, m, 16)) errs++;
        for (uint32_t k = 0; k < 16u; k++)
            if (sd_read(lba + k, s1 + k * 512u, 1)) errs++;
        if (memcmp(m, s1, 16 * 512)) {
            if (bad < 5) {
                uint32_t i = 0;
                while (m[i] == s1[i]) i++;
                printf("sdtest: lba %lu+: differ at byte %lu (%02x vs %02x)\n", (unsigned long)lba,
                       (unsigned long)i, m[i], s1[i]);
            }
            bad++;
        }
    }
    printf("sdtest: 8192 sectors, CMD18 vs CMD17: %lu groups of 16 differ, %lu errors\n",
           (unsigned long)bad, (unsigned long)errs);
    file_sum("DOOMS/DOOM1.WAD", 65536, m + 1);   // unaligned buffer
    file_sum("DOOMS/DOOM1.WAD", 1000, m);
    file_sum("DOOMS/DOOM1.WAD", 512, m);
    free(m);
    free(s1);
}
#endif
