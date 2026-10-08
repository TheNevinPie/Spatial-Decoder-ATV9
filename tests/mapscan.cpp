// mapscan.cpp — find our patched bytes in THIS process's memory.
// dlopens the plugin lib, then scans /proc/self/maps for the lib path and
// memmem-searches each readable mapping for three known-plaintext patterns:
//   TABLE  10B: 61 0A 65 10 64 17 74 1D FF 00
//   PREFIX 12B: "OMX.spatial."
//   CAVE   first 8B: 74 46 44 F2 BB 70 C0 F2 (mov r4,lr; movw r0,#0x47BB)
// Prints runtime addr, mapping perms/fileoff, and addr-minus-dlbase deltas.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char kTable[10] = {
    0x61, 0x0A, 0x65, 0x10, 0x64, 0x17, 0x74, 0x1D, 0xFF, 0x00,
};
static const unsigned char kPrefix[12] = {
    'O', 'M', 'X', '.', 's', 'p', 'a', 't', 'i', 'a', 'l', '.',
};
static const unsigned char kCave[8] = {
    0x74, 0x46, 0x44, 0xF2, 0xBB, 0x70, 0xC0, 0xF2,
};
static const unsigned char kCtl[] = "OMX.google.mp3.decoder";

static void scan(const char *lib, const unsigned char *pat, int patlen,
                 const char *tag) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        printf("SCAN %s FAIL maps\n", tag);
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, lib) == NULL) {
            continue;
        }
        unsigned long lo, hi, off;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s %lx", &lo, &hi, perms, &off) != 4) {
            continue;
        }
        if (perms[0] != 'r') {
            continue;
        }
        unsigned char *base = (unsigned char *)(uintptr_t)lo;
        size_t len = (size_t)(hi - lo);
        for (size_t i = 0; i + (size_t)patlen <= len; i++) {
            if (memcmp(base + i, pat, patlen) == 0) {
                printf("SCAN %s HIT addr=0x%08lx map=%lx-%lx perms=%s mapoff=0x%lx\n",
                        tag, (unsigned long)(base + i), lo, hi, perms, off);
                // Hexdump context: 64B at cave hits, 32B elsewhere.
                size_t dump =
                        (strcmp(tag, "cave") == 0) ? 64 : 32;
                if (i + dump <= len) {
                    printf("SCAN %s DUMP", tag);
                    for (size_t k = 0; k < dump; k++) {
                        printf(" %02x", base[i + k]);
                    }
                    printf("\n");
                }
            }
        }
        printf("SCAN %s map %lx-%lx perms=%s mapoff=0x%lx (searched)\n",
                tag, lo, hi, perms, off);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    const char *plugPath = (argc > 1)
            ? argv[1]
            : "/data/local/tmp/patchtest/libstagefright_omx.so";
    void *h = dlopen(plugPath, RTLD_NOW);
    if (h == NULL) {
        printf("SCAN FAIL dlopen: %s\n", dlerror());
        return 0;
    }
    void *make = dlsym(h,
            "_ZN7android13SoftOMXPlugin21makeComponentInstanceEPKcPK16OMX_CALLBACKTYPEPvPP17OMX_COMPONENTTYPE");
    Dl_info mi;
    memset(&mi, 0, sizeof(mi));
    if (dladdr(make, &mi) != 0) {
        printf("SCAN lib=%s fbase=0x%08lx saddr=0x%08lx\n", plugPath,
                (unsigned long)(uintptr_t)mi.dli_fbase,
                (unsigned long)(uintptr_t)mi.dli_saddr);
    }
    scan(plugPath, kTable, sizeof(kTable), "table");
    scan(plugPath, kPrefix, sizeof(kPrefix), "prefix");
    scan(plugPath, kCave, sizeof(kCave), "cave");
    scan(plugPath, kCtl, sizeof(kCtl) - 1, "control");
    return 0;
}
