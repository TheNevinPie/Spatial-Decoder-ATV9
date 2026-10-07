// lr_probe: report the exact value BL puts in LR (bit0 set or not?).
// Usage: lr_probe ; prints PROBE lr=0x... (call site known link-time).
#include <stdio.h>

static unsigned lr_at(void) {
    unsigned v;
    __asm__ volatile(
        "bl 1f\n"
        "1:\n"
        "mov %0, lr\n"
        : "=r"(v)
        :
        : "lr", "memory");
    return v;
}

int main(void) {
    setbuf(stdout, NULL);
    unsigned a = (unsigned)(void *)lr_at;
    unsigned v = lr_at();
    printf("PROBE fn=%08x lr=%08x bit0=%d delta=%d\n", a, v, v & 1,
            (int)(v - a));
    return 0;
}
