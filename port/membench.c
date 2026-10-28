// Memory timing microbenchmarks, run once at startup (see README): what a
// data-cache hit and miss, a store miss and an instruction-cache miss cost,
// in CPU cycles (the clock counter counts CPU clock cycles).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m9b_map.h"

#define CLOCK_CNT (*(volatile uint32_t*)(M9B_TIMING + 0xCu))

// Dependent loads: p = *p around a ring with the given stride.
static uint32_t chase(uint32_t *buf, uint32_t bytes, uint32_t stride, uint32_t loads) {
    uint32_t n = bytes / stride;
    for (uint32_t i = 0; i < n; i++)
        buf[i * stride / 4u] = (uint32_t)(uintptr_t)&buf[((i + 1u) % n) * stride / 4u];
    volatile uint32_t *p = buf;
    for (uint32_t i = 0; i < n; i++) p = (volatile uint32_t *)(uintptr_t)*p;   // warm / settle
    uint32_t t0 = CLOCK_CNT;
    for (uint32_t i = 0; i < loads; i++) p = (volatile uint32_t *)(uintptr_t)*p;
    uint32_t t = CLOCK_CNT - t0;
    return t * 100u / loads;   // cycles x 100 per load
}

// 64 KB of straight-line code (16384 nops): every 64-byte line a fetch miss.
extern void nops64k(void);
asm(".section .text\n.align 6\nnops64k:\n.rept 16384\nnop\n.endr\nret\n");

// The same count of instructions in a small loop (cached).
static void nops_loop(void) {
    for (uint32_t i = 0; i < 1024u; i++)
        asm volatile(".rept 16\nnop\n.endr");
}

void membench(void) {
    uint32_t *buf = malloc(1u << 20);
    if (!buf) return;
    uint32_t hit  = chase(buf, 8192u, 64u, 20000u);
    uint32_t miss = chase(buf, 1u << 20, 64u, 20000u);
    uint32_t miss_w = 0;
    {   // stores, one per 64-byte line, over 1 MB twice (the second pass
        // evicts dirty lines: write-back + refill)
        volatile uint32_t *q = buf;
        for (uint32_t i = 0; i < (1u << 20) / 4u; i += 16u) q[i] = i;
        uint32_t t0 = CLOCK_CNT;
        for (uint32_t i = 0; i < (1u << 20) / 4u; i += 16u) q[i] = i + 1u;
        miss_w = (CLOCK_CNT - t0) * 100u / ((1u << 20) / 64u);
    }
    // Byte stores into a 320 x 200 screen: in row order, in column order (as
    // R_DrawColumn writes), and into 4 KB (all hits). Cycles x 100 per store.
    uint32_t st_row, st_col, st_hit;
    {
        volatile uint8_t *s = (volatile uint8_t *)buf;
        for (uint32_t i = 0; i < 64000u; i++) s[i] = 0;
        uint32_t t0 = CLOCK_CNT;
        for (uint32_t i = 0; i < 64000u; i++) s[i] = (uint8_t)i;
        st_row = (CLOCK_CNT - t0) * 100u / 64000u;
        t0 = CLOCK_CNT;
        for (uint32_t x = 0; x < 320u; x++)
            for (uint32_t y = 0; y < 64000u; y += 320u) s[x + y] = (uint8_t)y;
        st_col = (CLOCK_CNT - t0) * 100u / 64000u;
        t0 = CLOCK_CNT;
        for (uint32_t k = 0; k < 16u; k++)
            for (uint32_t i = 0; i < 4000u; i++) s[i] = (uint8_t)i;
        st_hit = (CLOCK_CNT - t0) * 100u / 64000u;
    }
    printf("membench: byte stores (cycles, with loop): row order %lu.%02lu, column order %lu.%02lu, hits %lu.%02lu\n",
           (unsigned long)(st_row / 100u), (unsigned long)(st_row % 100u),
           (unsigned long)(st_col / 100u), (unsigned long)(st_col % 100u),
           (unsigned long)(st_hit / 100u), (unsigned long)(st_hit % 100u));
    uint32_t t0 = CLOCK_CNT;
    nops64k();
    uint32_t t_line = (CLOCK_CNT - t0) * 100u / 1024u;   // per 64-byte line (16 nops)
    t0 = CLOCK_CNT;
    nops_loop();
    uint32_t t_loop = (CLOCK_CNT - t0) * 100u / 1024u;    // per 16 cached nops + loop
    // memset / memcpy (port/string.c) of 64 KB, CPU cycles per KB, each
    // starting with the data cache full of unrelated dirty lines.
    uint32_t t_set0, t_set, t_cpy;
    {
        uint8_t *b8 = (uint8_t *)buf;
        volatile uint32_t *q = buf;
        for (uint32_t i = 0; i < 16384u / 4u; i++) q[(768u << 10) / 4u + i] = i;   // dirty 16 KB
        uint32_t t1 = CLOCK_CNT;
        memset(b8, 0, 65536u);
        t_set0 = (CLOCK_CNT - t1) / 64u;
        for (uint32_t i = 0; i < 16384u / 4u; i++) q[(768u << 10) / 4u + i] = i;
        t1 = CLOCK_CNT;
        memset(b8 + 65536u, 0x5A, 65536u);
        t_set = (CLOCK_CNT - t1) / 64u;
        for (uint32_t i = 0; i < 16384u / 4u; i++) q[(768u << 10) / 4u + i] = i;
        t1 = CLOCK_CNT;
        // (through a volatile pointer: GCC otherwise drops the copy, as it
        // knows what the check below will read)
        void *(*volatile cpy)(void *, const void *, size_t) = memcpy;
        cpy(b8 + (256u << 10) + 8192u, b8 + 65536u, 65536u);   // other cache sets
        t_cpy = (CLOCK_CNT - t1) / 64u;
        if (b8[(256u << 10) + 8192u + 1000u] != 0x5A || b8[1000u] != 0) t_cpy = 0xFFFFFFFFu;
    }
    printf("membench: 64 KB in CPU cycles per KB: memset(0) %lu, memset(x) %lu, memcpy %lu\n",
           (unsigned long)t_set0, (unsigned long)t_set, (unsigned long)t_cpy);
    free(buf);
    printf("membench (CPU cycles): load hit %lu.%02lu, load miss %lu.%02lu, "
           "store miss (dirty evict) %lu.%02lu, fetch 16 nops: missing %lu.%02lu, cached %lu.%02lu\n",
           (unsigned long)(hit / 100u), (unsigned long)(hit % 100u),
           (unsigned long)(miss / 100u), (unsigned long)(miss % 100u),
           (unsigned long)(miss_w / 100u), (unsigned long)(miss_w % 100u),
           (unsigned long)(t_line / 100u), (unsigned long)(t_line % 100u),
           (unsigned long)(t_loop / 100u), (unsigned long)(t_loop % 100u));
}
