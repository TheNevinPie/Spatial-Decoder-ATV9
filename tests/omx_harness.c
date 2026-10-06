// omx_harness.c — standalone Stage-2 factory/lifecycle harness (device-side).
//
// Usage (on MT5862 TV via adb shell):
//   LD_LIBRARY_PATH=<dir with FFmpeg 8.1> omx_harness <adapter-dir>
//
// For each libstagefright_soft_{ac3,eac3,dts,truehd}dec.so it:
//   1. dlopen()s the library.
//   2. dlsym()s the MANGLED global factory
//      _Z22createSoftOMXComponentPKcPK16OMX_CALLBACKTYPEPvPP17OMX_COMPONENTTYPE
//      (the exact symbol SoftOMXPlugin looks up) plus the namespaced
//      android::createSoftAc3OmxComponent for symbol-presence.
//   3. Calls the factory with the matching OMX.google.*.decoder name.
//   4. Exercises GetState (expect Loaded) and GetParameter for
//      StandardComponentRole + AudioPcm (expect stereo S16 @48k).
//   5. _exit()s WITHOUT destroy (teardown belongs to SoftOMXPlugin's
//      sp<> ownership in production; leaking one component per lib in a
//      short-lived test process is intentional).
//
// Exit 0 iff every lib passes every step.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OMX_Component.h>
#include <OMX_Core.h>
#include <OMX_Index.h>
#include <OMX_Audio.h>

#define FACTORY_MANGLED \
    "_Z22createSoftOMXComponentPKcPK16OMX_CALLBACKTYPEPvPP17OMX_COMPONENTTYPE"
#define FACTORY_NAMESPACED \
    "_ZN7android25createSoftAc3OmxComponentEPKcPK16OMX_CALLBACKTYPEPvPP17OMX_COMPONENTTYPE"

typedef void *(*FactoryFunc)(
        const char *, const OMX_CALLBACKTYPE *, OMX_PTR, OMX_COMPONENTTYPE **);

static const struct {
    const char *file;
    const char *compName;
    const char *expectRole;
} kLibs[] = {
    { "libstagefright_soft_ac3dec.so", "OMX.google.ac3.decoder", "audio_decoder.ac3" },
    { "libstagefright_soft_eac3dec.so", "OMX.google.eac3.decoder", "audio_decoder.eac3" },
    { "libstagefright_soft_dtsdec.so", "OMX.google.dts.decoder", "audio_decoder.dts" },
    { "libstagefright_soft_truehddec.so", "OMX.google.truehd.decoder", "audio_decoder.truehd" },
};

static int g_failures = 0;

#define CHECK(cond, fmt, ...) do { \
    if (cond) { \
        printf("  PASS: " fmt "\n", ##__VA_ARGS__); \
    } else { \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
        g_failures++; \
    } \
} while (0)

static void test_one(const char *dir, const char *file, const char *compName,
        const char *expectRole) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    printf("== %s (%s) ==\n", file, compName);

    void *handle = dlopen(path, RTLD_NOW);
    CHECK(handle != NULL, "dlopen %s (%s)", path,
            handle ? "ok" : dlerror());
    if (!handle) {
        return;
    }

    CHECK(dlsym(handle, FACTORY_NAMESPACED) != NULL,
            "namespaced factory present");
    FactoryFunc factory = (FactoryFunc)dlsym(handle, FACTORY_MANGLED);
    CHECK(factory != NULL, "global factory %s (%s)", FACTORY_MANGLED,
            factory ? "ok" : dlerror());
    if (!factory) {
        return;
    }

    OMX_CALLBACKTYPE cbs;
    memset(&cbs, 0, sizeof(cbs));
    OMX_COMPONENTTYPE *comp = NULL;
    void *obj = factory(compName, &cbs, NULL, &comp);
    CHECK(obj != NULL, "factory returned object");
    CHECK(comp != NULL, "factory filled OMX_COMPONENTTYPE*");
    if (!obj || !comp) {
        return;
    }

    OMX_STATETYPE state = (OMX_STATETYPE)0;
    OMX_ERRORTYPE err = comp->GetState(comp, &state);
    CHECK(err == OMX_ErrorNone && state == OMX_StateLoaded,
            "GetState -> err=0x%x state=%d (want Loaded=1)", err, state);

    OMX_PARAM_COMPONENTROLETYPE role;
    memset(&role, 0, sizeof(role));
    role.nSize = sizeof(role);
    role.nVersion.s.nVersionMajor = 1;
    err = comp->GetParameter(comp, (OMX_INDEXTYPE)0x01000017, &role);
    CHECK(err == OMX_ErrorNone && strcmp((const char *)role.cRole, expectRole) == 0,
            "Role -> err=0x%x role='%s' (want '%s')", err, role.cRole, expectRole);

    OMX_AUDIO_PARAM_PCMMODETYPE pcm;
    memset(&pcm, 0, sizeof(pcm));
    pcm.nSize = sizeof(pcm);
    pcm.nVersion.s.nVersionMajor = 1;
    pcm.nPortIndex = 1;
    err = comp->GetParameter(comp, (OMX_INDEXTYPE)0x04000002, &pcm);
    CHECK(err == OMX_ErrorNone, "PCM GetParameter -> err=0x%x", err);
    if (err == OMX_ErrorNone) {
        CHECK(pcm.nChannels == 2 && pcm.nSamplingRate == 48000
                && pcm.nBitPerSample == 16 && pcm.bInterleaved == OMX_TRUE,
                "PCM stereo S16 interleaved 48k (ch=%u rate=%u bits=%u inter=%d)",
                pcm.nChannels, pcm.nSamplingRate, pcm.nBitPerSample,
                pcm.bInterleaved);
        CHECK(pcm.eChannelMapping[0] == OMX_AUDIO_ChannelLF
                && pcm.eChannelMapping[1] == OMX_AUDIO_ChannelRF,
                "PCM mapping LF/RF (got %u/%u)",
                pcm.eChannelMapping[0], pcm.eChannelMapping[1]);
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    printf("OMX adapter harness, dir=%s\n", dir);
    for (size_t i = 0; i < sizeof(kLibs) / sizeof(kLibs[0]); i++) {
        test_one(dir, kLibs[i].file, kLibs[i].compName, kLibs[i].expectRole);
    }
    if (g_failures == 0) {
        printf("ALL HARNESS CHECKS PASSED\n");
    } else {
        printf("%d HARNESS CHECK(S) FAILED\n", g_failures);
    }
    fflush(stdout);
    _exit(g_failures == 0 ? 0 : 1);
    return g_failures == 0 ? 0 : 1;
}
