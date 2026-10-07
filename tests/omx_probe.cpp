// omx_probe.cpp — component-name resolution boundary probe (device-side).
//
// Exercises the REAL lookup paths for each OMX.test.spatial.* name:
//   (A) app level: AMediaCodec_createByCodecName (full MediaCodecList +
//       OMX service routing, same as any player).
//   (B) app level: AMediaCodec_createDecoderByType for the four MIME
//       types (shows which decoder backs each type today; verifies the
//       stock google routes are unchanged).
//   (C) plugin level: MTK SoftOMXPlugin::makeComponentInstance via the
//       real /system/lib/libstagefright_omx.so (exact OMX_ERRORTYPE).
//       Includes OMX.google.mp3.decoder as a positive control.
//
// Usage: omx_probe
// Exit 0 always (this is a reporter, not a pass/fail gate); PROBE lines
// on stdout (unbuffered). Links mediandk + dl only.

#include <media/NdkMediaCodec.h>

#include <aaudio/AAudio.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OMX_Component.h>
#include <OMX_Core.h>

static const char *omx_err_name(int32_t e) {
    switch ((uint32_t)e) {
        case 0x00000000: return "None";
        case 0x80001001: return "InsufficientResources";
        case 0x80001003: return "ComponentNotFound";
        case 0x80001005: return "BadParameter";
        case 0x80001006: return "NotImplemented";
        default: return "?";
    }
}

typedef void *(*PluginCtorFn)(void *);
typedef int32_t (*PluginMakeFn)(
        void *, const char *, const OMX_CALLBACKTYPE *, OMX_PTR,
        OMX_COMPONENTTYPE **);
typedef int32_t (*PluginDestroyFn)(void *, OMX_COMPONENTTYPE *);

// Stub OMX callbacks: a created test component may emit events during
// the probe; it must never touch a NULL function pointer.
static OMX_ERRORTYPE stubEvent(
        OMX_HANDLETYPE h, OMX_PTR app, OMX_EVENTTYPE e, OMX_U32 d1,
        OMX_U32 d2, OMX_PTR data) {
    (void)h;
    (void)app;
    (void)e;
    (void)d1;
    (void)d2;
    (void)data;
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE stubEmpty(
        OMX_HANDLETYPE h, OMX_PTR app, OMX_BUFFERHEADERTYPE *b) {
    (void)h;
    (void)app;
    (void)b;
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE stubFill(
        OMX_HANDLETYPE h, OMX_PTR app, OMX_BUFFERHEADERTYPE *b) {
    (void)h;
    (void)app;
    (void)b;
    return OMX_ErrorNone;
}

int main(void) {
    setbuf(stdout, NULL);
    printf("PROBE pid=%d\n", (int)getpid());

    static const char *kTestNames[4] = {
        "OMX.test.spatial.ac3",
        "OMX.test.spatial.eac3",
        "OMX.test.spatial.dts",
        "OMX.test.spatial.truehd",
    };
    static const char *kMimes[4] = {
        "audio/ac3", "audio/eac3", "audio/vnd.dts", "audio/true-hd",
    };

    // (A) app-level by-name lookup.
    static const int kChannels[4] = { 6, 6, 6, 6 };
    static const int kBitrate[4] = { 448000, 1024000, 1536000, 6000000 };
    static const char *kFiles[4] = {
        "/data/local/tmp/test_audio/test_ac3_51.ac3",
        "/data/local/tmp/test_audio/test_eac3_51.ec3",
        "/data/local/tmp/test_audio/test_dts_51_chid.dts",
        "/data/local/tmp/test_audio/test_truehd_51_chid.thd",
    };
    static const char *kShort[4] = { "ac3", "eac3", "dts", "thd" };
    static const char *kEnv[4] = {
        "SPATIAL_FILE_AC3", "SPATIAL_FILE_EAC3",
        "SPATIAL_FILE_DTS", "SPATIAL_FILE_THD",
    };
    for (int i = 0; i < 4; i++) {
        AMediaCodec *c = AMediaCodec_createCodecByName(kTestNames[i]);
        printf("PROBE mediacodec-byname name=%s codec=%p\n",
                kTestNames[i], (void *)c);
        // Optional per-codec input override (channel-ID synthesis, etc.).
        const char *kPath = kFiles[i];
        {
            const char *ov = getenv(kEnv[i]);
            if (ov != NULL && ov[0] != '\0') {
                kPath = ov;
                printf("PROBE file name=%s path=%s\n", kTestNames[i], kPath);
            }
        }
        if (c != NULL) {
            // Component initialization: configure + start. Syncframe
            // codecs need csd-0 (ACodec rejects csd-less formats); feed
            // the file's first 256 bytes as csd-0 (parser-grade payload;
            // our decoder is parser-driven and tolerates it).
            AMediaFormat *fmt = AMediaFormat_new();
            AMediaFormat_setString(fmt, "mime", kMimes[i]);
            AMediaFormat_setInt32(fmt, "sample-rate", 48000);
            {
                // Env knobs for fast configure matrix (no rebuilds):
                // SPATIAL_CH (default 6), SPATIAL_CSD (0/1, default 1),
                // SPATIAL_CSDN (bytes 0..256, default 256).
                int ch = 6, csd = 1, csdn = 256;
                const char *e;
                if ((e = getenv("SPATIAL_CH")) != NULL) {
                    ch = atoi(e);
                }
                if ((e = getenv("SPATIAL_CSD")) != NULL) {
                    csd = atoi(e);
                }
                if ((e = getenv("SPATIAL_CSDN")) != NULL) {
                    csdn = atoi(e);
                    if (csdn < 0) {
                        csdn = 0;
                    }
                    if (csdn > 256) {
                        csdn = 256;
                    }
                }
                AMediaFormat_setInt32(fmt, "channel-count", ch);
                AMediaFormat_setInt32(fmt, "bitrate", kBitrate[i]);
                printf("PROBE init name=%s ch=%d csd=%d csdn=%d\n",
                        kTestNames[i], ch, csd, csdn);
                if (csd) {
                    FILE *f = fopen(kPath, "rb");
                    if (f != NULL) {
                        static uint8_t csdbuf[256];
                        size_t n = fread(csdbuf, 1, (size_t)csdn, f);
                        fclose(f);
                        if (n > 0) {
                            AMediaFormat_setBuffer(fmt, "csd-0", csdbuf, n);
                        }
                    }
                }
            }
            media_status_t cs =
                    AMediaCodec_configure(c, fmt, NULL, NULL, 0);
            printf("PROBE init name=%s configure=%d\n", kTestNames[i],
                    (int)cs);
            media_status_t ss = AMEDIA_ERROR_UNKNOWN;
            if (cs == AMEDIA_OK) {
                ss = AMediaCodec_start(c);
                printf("PROBE init name=%s start=%d\n", kTestNames[i],
                        (int)ss);
            }
            if (ss == AMEDIA_OK) {
                // (G) end-to-end decode proof: feed the whole test file
                // through input buffers, drain output, count PCM bytes,
                // dump raw PCM to file, capture output format.
                // Bounded loops only (10 ms dequeue timeouts, hard iter
                // cap). File reopened fresh so decoding starts at frame 0.
                FILE *df = fopen(kPath, "rb");
                char pcmPath[128];
                snprintf(pcmPath, sizeof(pcmPath),
                        "/data/local/tmp/pcm_%s.pcm", kShort[i]);
                FILE *pf = fopen(pcmPath, "wb");
                uint64_t inBytes = 0, outBytes = 0;
                unsigned inBufs = 0, outBufs = 0;
                int64_t pts = 0;
                bool sawEos = false;
                bool inEos = false;
                uint32_t xorFold = 0;
                uint8_t firstOut[16];
                unsigned firstOutN = 0;
                int32_t fmtRate = 48000, fmtCh = 2, fmtEnc = 2;
                for (unsigned iter = 0; iter < 1500 && !sawEos; iter++) {
                    if (df != NULL && !inEos) {
                        ssize_t ii = AMediaCodec_dequeueInputBuffer(
                                c, 10000);
                        if (ii >= 0) {
                            size_t cap = 0;
                            uint8_t *ib = AMediaCodec_getInputBuffer(
                                    c, (size_t)ii, &cap);
                            size_t n = 0;
                            if (ib != NULL && cap > 0) {
                                size_t want = cap < 4096 ? cap : 4096;
                                n = fread(ib, 1, want, df);
                            }
                            uint32_t flags = 0;
                            if (n == 0) {
                                inEos = true;
                                flags =
                                    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
                            }
                            AMediaCodec_queueInputBuffer(
                                    c, (size_t)ii, 0, n, pts, flags);
                            pts += 20000;
                            inBytes += (uint64_t)n;
                            inBufs++;
                        }
                    }
                    AMediaCodecBufferInfo info;
                    memset(&info, 0, sizeof(info));
                    ssize_t oi = AMediaCodec_dequeueOutputBuffer(
                            c, &info, 10000);
                    if (oi >= 0) {
                        size_t osz = 0;
                        uint8_t *ob = AMediaCodec_getOutputBuffer(
                                c, (size_t)oi, &osz);
                        if (ob != NULL && info.size > 0) {
                            uint32_t lim = (uint32_t)info.size;
                            if ((size_t)lim > osz) {
                                lim = (uint32_t)osz;
                            }
                            for (uint32_t k = 0; k < lim; k++) {
                                xorFold ^=
                                    (uint32_t)ob[k] << ((k & 3) * 8);
                                if (firstOutN < sizeof(firstOut)) {
                                    firstOut[firstOutN++] = ob[k];
                                }
                            }
                            if (pf != NULL) {
                                fwrite(ob, 1, lim, pf);
                            }
                            outBytes += (uint64_t)lim;
                        }
                        outBufs++;
                        if (info.flags &
                                AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                            sawEos = true;
                        }
                        AMediaCodec_releaseOutputBuffer(
                                c, (size_t)oi, false);
                    } else if (oi ==
                            AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                        AMediaFormat *of = AMediaCodec_getOutputFormat(c);
                        if (of != NULL) {
                            AMediaFormat_getInt32(
                                    of, "sample-rate", &fmtRate);
                            AMediaFormat_getInt32(
                                    of, "channel-count", &fmtCh);
                            AMediaFormat_getInt32(
                                    of, "pcm-encoding", &fmtEnc);
                            AMediaFormat_delete(of);
                        }
                    }
                }
                if (df != NULL) {
                    fclose(df);
                }
                if (pf != NULL) {
                    fclose(pf);
                }
                printf("PROBE decode name=%s inBytes=%llu outBytes=%llu "
                        "inBufs=%u outBufs=%u xor=%08x eos=%d "
                        "rate=%d ch=%d enc=%d first=",
                        kTestNames[i], (unsigned long long)inBytes,
                        (unsigned long long)outBytes, inBufs, outBufs,
                        xorFold, (int)sawEos, (int)fmtRate, (int)fmtCh,
                        (int)fmtEnc);
                for (unsigned h = 0; h < firstOutN && h < 16; h++) {
                    printf("%02x", firstOut[h]);
                }
                printf("\n");
                AMediaCodec_stop(c);
            }
            AMediaFormat_delete(fmt);
            AMediaCodec_delete(c);
            if (ss == AMEDIA_OK) {
                // (H) production audio path: render the dumped PCM via
                // AAudio (shared mixer -> HAL). Bounded blocking writes;
                // xrun counter proves underrun-free delivery. Audibility
                // itself needs a listener and stays UNTESTED here.
                char pcmPath[128];
                snprintf(pcmPath, sizeof(pcmPath),
                        "/data/local/tmp/pcm_%s.pcm", kShort[i]);
                FILE *pf = fopen(pcmPath, "rb");
                AAudioStreamBuilder *bld = NULL;
                AAudioStream *st = NULL;
                int64_t framesOut = 0;
                int32_t xruns = -2;
                int32_t openErr = -1;
                if (AAudio_createStreamBuilder(&bld) == AAUDIO_OK) {
                    AAudioStreamBuilder_setDirection(
                            bld, AAUDIO_DIRECTION_OUTPUT);
                    AAudioStreamBuilder_setSharingMode(
                            bld, AAUDIO_SHARING_MODE_SHARED);
                    AAudioStreamBuilder_setFormat(
                            bld, AAUDIO_FORMAT_PCM_I16);
                    AAudioStreamBuilder_setChannelCount(bld, 2);
                    AAudioStreamBuilder_setSampleRate(bld, 48000);
                    AAudioStreamBuilder_setPerformanceMode(
                            bld, AAUDIO_PERFORMANCE_MODE_NONE);
                    openErr = AAudioStreamBuilder_openStream(bld, &st);
                    AAudioStreamBuilder_delete(bld);
                    bld = NULL;
                }
                if (openErr == AAUDIO_OK && st != NULL && pf != NULL) {
                    static int16_t pcmChunk[8192];
                    size_t nr;
                    while ((nr = fread(pcmChunk, 1, sizeof(pcmChunk),
                            pf)) > 0) {
                        int32_t frames =
                                (int32_t)(nr / sizeof(int16_t) / 2);
                        if (frames <= 0) {
                            break;
                        }
                        aaudio_result_t wr = AAudioStream_write(
                                st, pcmChunk, frames, 500000000);
                        if (wr < 0) {
                            openErr = (int32_t)wr;
                            break;
                        }
                        framesOut += wr;
                    }
                    xruns = AAudioStream_getXRunCount(st);
                    AAudioStream_close(st);
                }
                if (pf != NULL) {
                    fclose(pf);
                }
                printf("PROBE render name=%s open=%d frames=%lld xruns=%d\n",
                        kTestNames[i], (int)openErr,
                        (long long)framesOut, (int)xruns);
            }
        }
    }

    // (I) flush/seek/lifecycle soak: 3 rounds per codec of
    // create/configure/start/feed-half/flush(seek jump)/feed-rest/
    // drain-to-EOS/stop/delete. Verifies parser+decoder+timestamp
    // reset, no stale frames, stable repeated teardown.
    for (int i = 0; i < 4; i++) {
        const char *ipath = kFiles[i];
        {
            const char *ov = getenv(kEnv[i]);
            if (ov != NULL && ov[0] != '\0') {
                ipath = ov;
            }
        }
        for (int round = 0; round < 3; round++) {
            AMediaCodec *c = AMediaCodec_createCodecByName(kTestNames[i]);
            uint64_t fed = 0, got = 0;
            int64_t firstPostPts = -1, lastPts = -1;
            bool eos = false, inEos = false, flushed = false;
            int errs = 0;
            int csR = -999, ssR = -999, flR = -999;
            if (c != NULL) {
                AMediaFormat *fmt = AMediaFormat_new();
                AMediaFormat_setString(fmt, "mime", kMimes[i]);
                AMediaFormat_setInt32(fmt, "sample-rate", 48000);
                AMediaFormat_setInt32(fmt, "channel-count", 6);
                AMediaFormat_setInt32(fmt, "bitrate", kBitrate[i]);
                {
                    FILE *f = fopen(ipath, "rb");
                    if (f != NULL) {
                        static uint8_t csd[256];
                        size_t n = fread(csd, 1, sizeof(csd), f);
                        fclose(f);
                        if (n > 0) {
                            AMediaFormat_setBuffer(fmt, "csd-0", csd, n);
                        }
                    }
                }
                csR = (int)AMediaCodec_configure(c, fmt, NULL, NULL, 0);
                AMediaFormat_delete(fmt);
                if (csR == AMEDIA_OK) {
                    ssR = (int)AMediaCodec_start(c);
                }
                FILE *df = NULL;
                long fsize = 0, halfAt = 0;
                int64_t pts = 0;
                if (ssR == AMEDIA_OK) {
                    df = fopen(ipath, "rb");
                    if (df != NULL) {
                        fseek(df, 0, SEEK_END);
                        fsize = ftell(df);
                        fseek(df, 0, SEEK_SET);
                        halfAt = fsize / 2;
                    }
                }
                for (unsigned it = 0;
                        it < 1500 && ssR == AMEDIA_OK && !eos; it++) {
                    if (df != NULL && !inEos) {
                        ssize_t ii = AMediaCodec_dequeueInputBuffer(
                                c, 10000);
                        if (ii >= 0) {
                            size_t cap = 0;
                            uint8_t *ib = AMediaCodec_getInputBuffer(
                                    c, (size_t)ii, &cap);
                            size_t n = 0;
                            if (ib != NULL && cap > 0) {
                                size_t want = cap < 4096 ? cap : 4096;
                                n = fread(ib, 1, want, df);
                            }
                            uint32_t fl = 0;
                            if (n == 0) {
                                inEos = true;
                                fl = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
                            }
                            if (AMediaCodec_queueInputBuffer(
                                    c, (size_t)ii, 0, n, pts,
                                    fl) != AMEDIA_OK) {
                                errs++;
                            }
                            pts += 20000;
                            fed += (uint64_t)n;
                            if (!flushed && fed >= (uint64_t)halfAt &&
                                    halfAt > 0) {
                                flR = (int)AMediaCodec_flush(c);
                                flushed = true;
                                pts = 10000000LL * (round + 1);
                            }
                        } else if (ii !=
                                AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                            errs++;
                        }
                    }
                    AMediaCodecBufferInfo bi;
                    memset(&bi, 0, sizeof(bi));
                    ssize_t oi = AMediaCodec_dequeueOutputBuffer(
                            c, &bi, 10000);
                    if (oi >= 0) {
                        if (bi.size > 0) {
                            got += (uint64_t)bi.size;
                        }
                        lastPts = bi.presentationTimeUs;
                        if (flushed && firstPostPts < 0) {
                            firstPostPts = bi.presentationTimeUs;
                        }
                        if (bi.flags &
                                AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                            eos = true;
                        }
                        AMediaCodec_releaseOutputBuffer(
                                c, (size_t)oi, false);
                    } else if (oi != AMEDIACODEC_INFO_TRY_AGAIN_LATER &&
                            oi != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED &&
                            oi != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                        errs++;
                    }
                }
                if (df != NULL) {
                    fclose(df);
                }
                if (ssR == AMEDIA_OK) {
                    AMediaCodec_stop(c);
                }
                AMediaCodec_delete(c);
            }
            printf("PROBE soak name=%s round=%d cs=%d ss=%d flush=%d "
                    "fed=%llu got=%llu firstPostPts=%lld lastPts=%lld "
                    "eos=%d errs=%d\n",
                    kTestNames[i], round, csR, ssR, flR,
                    (unsigned long long)fed, (unsigned long long)got,
                    (long long)firstPostPts, (long long)lastPts,
                    (int)eos, errs);
        }
    }

    // (B) app-level by-type lookup (current routing per MIME).
    for (int i = 0; i < 4; i++) {
        AMediaCodec *c = AMediaCodec_createDecoderByType(kMimes[i]);
        printf("PROBE mediacodec-bytype mime=%s codec=%p\n",
                kMimes[i], (void *)c);
        if (c != NULL) {
            AMediaCodec_delete(c);
        }
    }
    static const char *kStockMime[2] = { "audio/mpeg", "audio/mp4a-latm" };
    static const char *kStockFile[2] = {
        "/data/local/tmp/test_audio/test_stereo.mp3",
        "/data/local/tmp/test_audio/test_stereo.aac",
    };
    static const int kStockRate[2] = { 44100, 44100 };
    for (int si = 0; si < 2; si++) {
        AMediaCodec *c = AMediaCodec_createDecoderByType(kStockMime[si]);
        printf("PROBE mediacodec-bytype mime=%s codec=%p (control)\n",
                kStockMime[si], (void *)c);
        if (c != NULL) {
            char *mname = NULL;
            if (AMediaCodec_getName(c, &mname) == AMEDIA_OK &&
                    mname != NULL) {
                printf("PROBE stock component name=%s\n", mname);
                AMediaCodec_releaseName(c, mname);
            }
            AMediaFormat *fmt = AMediaFormat_new();
            AMediaFormat_setString(fmt, "mime", kStockMime[si]);
            AMediaFormat_setInt32(fmt, "sample-rate", kStockRate[si]);
            AMediaFormat_setInt32(fmt, "channel-count", 2);
            // NOTE: no csd-0 here on purpose. Raw ADTS/MP3 frames are
            // self-synchronizing; stuffing file bytes as CODEC_CONFIG
            // poisons stock setup (proven: zero output with csd-0).
            media_status_t mcs =
                    AMediaCodec_configure(c, fmt, NULL, NULL, 0);
            printf("PROBE stock mime=%s configure=%d\n",
                    kStockMime[si], (int)mcs);
            if (mcs == AMEDIA_OK) {
                media_status_t mss = AMediaCodec_start(c);
                printf("PROBE stock mime=%s start=%d\n",
                        kStockMime[si], (int)mss);
                if (mss == AMEDIA_OK) {
                    FILE *mf = fopen(kStockFile[si], "rb");
                    uint64_t mOut = 0, mIn = 0;
                    unsigned mBufs = 0, mInBufs = 0;
                    bool mEos = false;
                    bool mInEos = false;
                    int64_t mpts = 0;
                    for (unsigned it = 0;
                            it < 600 && !mEos; it++) {
                        if (mf != NULL && !mInEos) {
                            ssize_t ii = AMediaCodec_dequeueInputBuffer(
                                    c, 10000);
                            if (ii >= 0) {
                                size_t cap = 0;
                                uint8_t *ib = AMediaCodec_getInputBuffer(
                                        c, (size_t)ii, &cap);
                                size_t n = 0;
                                if (ib != NULL && cap > 0) {
                                    size_t want =
                                            cap < 4096 ? cap : 4096;
                                    n = fread(ib, 1, want, mf);
                                }
                                uint32_t fl = 0;
                                if (n == 0) {
                                    mInEos = true;
                                    fl = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
                                }
                                AMediaCodec_queueInputBuffer(
                                        c, (size_t)ii, 0, n, mpts, fl);
                                mpts += 20000;
                                mIn += (uint64_t)n;
                                mInBufs++;
                            }
                        }
                        AMediaCodecBufferInfo oi;
                        memset(&oi, 0, sizeof(oi));
                        ssize_t oo = AMediaCodec_dequeueOutputBuffer(
                                c, &oi, 10000);
                        if (oo >= 0) {
                            if (oi.size > 0) {
                                mOut += (uint64_t)oi.size;
                            }
                            mBufs++;
                            if (oi.flags &
                                    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                                mEos = true;
                            }
                            AMediaCodec_releaseOutputBuffer(
                                    c, (size_t)oo, false);
                        }
                    }
                    if (mf != NULL) {
                        fclose(mf);
                    }
                    printf("PROBE stock mime=%s outBytes=%llu "
                            "outBufs=%u inBytes=%llu inBufs=%u nofile=%d\n",
                            kStockMime[si], (unsigned long long)mOut, mBufs,
                            (unsigned long long)mIn, mInBufs,
                            (int)(mf == NULL));
                }
                AMediaCodec_stop(c);
            }
            AMediaFormat_delete(fmt);
            AMediaCodec_delete(c);
        }
    }

    // (C) plugin-level exact error codes. PROBE_PLUGIN_PATH overrides
    // the plugin library path (default: live /system/lib/...).
    const char *plugPath = getenv("PROBE_PLUGIN_PATH");
    if (plugPath == NULL) {
        plugPath = "/system/lib/libstagefright_omx.so";
    }
    printf("PROBE plugin path=%s\n", plugPath);
    void *h = dlopen(plugPath, RTLD_NOW);
    if (h == NULL) {
        printf("PROBE plugin FAIL dlopen: %s\n", dlerror());
        return 0;
    }
    PluginCtorFn ctor = (PluginCtorFn)dlsym(
            h, "_ZN7android13SoftOMXPluginC1Ev");
    PluginMakeFn make = (PluginMakeFn)dlsym(
            h, "_ZN7android13SoftOMXPlugin21makeComponentInstanceEPKcPK16OMX_CALLBACKTYPEPvPP17OMX_COMPONENTTYPE");
    PluginDestroyFn destroy = (PluginDestroyFn)dlsym(
            h, "_ZN7android13SoftOMXPlugin20destroyComponentInstanceEP17OMX_COMPONENTTYPE");
    if (ctor == NULL || make == NULL) {
        printf("PROBE plugin FAIL symbols ctor=%p make=%p\n",
                (void *)ctor, (void *)make);
        return 0;
    }
    {
        // Diagnostic: verify cave address math end-to-end WITHOUT
        // executing the cave. Derive the load bias from the make symbol,
        // then read what the cave would read (TABLE, prefix, entries).
        Dl_info mi;
        memset(&mi, 0, sizeof(mi));
        if (dladdr((void *)make, &mi) != 0) {
            // True load bias = saddr(make) - st_value(0x2C364). dli_fbase
            // is the FIRST-SEGMENT map addr (bias+0x3000 here), NOT the
            // bias -- do not use it. Proven by mapscan cave HIT at
            // mapbase+fileoff consistent with this bias.
            uintptr_t base =
                    ((uintptr_t)mi.dli_saddr & ~(uintptr_t)1)
                    - (uintptr_t)0x2C364;
            printf("PROBE addrmap fname=%s saddr=0x%08lx bias=0x%08lx\n",
                    mi.dli_fname ? mi.dli_fname : "?",
                    (unsigned long)(uintptr_t)mi.dli_saddr,
                    (unsigned long)base);
            volatile const unsigned char *tab =
                    (const unsigned char *)(base + 0x40B4A);
            volatile const unsigned char *pre =
                    (const unsigned char *)(base + 0x40B42);
            char pfx[9];
            memcpy(pfx, (const void *)pre, 8);
            pfx[8] = 0;
            printf("PROBE addrmap base=0x%08lx table=%p prefix='%s'\n",
                    (unsigned long)base, (const void *)tab, pfx);
            for (int k = 0; k < 5; k++) {
                printf("PROBE addrmap entry%d char=0x%02x liboff=0x%x\n",
                        k, tab[2 * k], tab[2 * k + 1]);
            }
            {
                // GOT slot for strncmp (link-time 0x48B00): what does the
                // PLT stub actually jump through at runtime? Guarded by
                // the prefix check above -- never touch wild addresses.
                if (memcmp(pfx, "OMX.test", 8) == 0) {
                    uintptr_t got = 0;
                    memcpy(&got, (const void *)(base + 0x48B00), 4);
                    printf("PROBE addrmap strncmp-got=0x%08lx\n",
                            (unsigned long)got);
                } else {
                    printf("PROBE addrmap SKIP got (prefix mismatch)\n");
                }
            }
        }
    }
    void *plug = calloc(1, 4096);
    if (plug == NULL) {
        printf("PROBE plugin FAIL nomem\n");
        return 0;
    }
    ctor(plug);
    OMX_CALLBACKTYPE cb;
    memset(&cb, 0, sizeof(cb));
    cb.EventHandler = stubEvent;
    cb.EmptyBufferDone = stubEmpty;
    cb.FillBufferDone = stubFill;
    for (int i = 0; i < 4; i++) {
        OMX_COMPONENTTYPE *comp = NULL;
        int32_t err = make(plug, kTestNames[i], &cb, NULL, &comp);
        printf("PROBE plugin name=%s err=0x%08x(%s) comp=%p\n",
                kTestNames[i], (uint32_t)err, omx_err_name(err),
                (void *)comp);
        if (err == 0 && comp != NULL) {
            uint32_t vtab = 0;
            memcpy(&vtab, (const void *)comp, 4);
            printf("PROBE plugin name=%s vtable=0x%08x\n",
                    kTestNames[i], vtab);
            // Locate the mSpatialCookie magic (0x53504154) in the adapter
            // object for the destroy-cave offset.
            const volatile uint32_t *obj = NULL;
            memcpy(&obj, (const uint8_t *)comp + 8, 4);
            if (obj != NULL) {
                for (int w = 0; w < 96; w++) {
                    uint32_t v = 0;
                    memcpy(&v, (const uint8_t *)obj + w * 4, 4);
                    if (v == 0x53504154u) {
                        printf("PROBE plugin name=%s cookie at obj+%d\n",
                                kTestNames[i], w * 4);
                        break;
                    }
                }
            }
        }
        if (err == 0 && comp != NULL && destroy != NULL) {
            destroy(plug, comp);
        }
    }
    {
        OMX_COMPONENTTYPE *comp = NULL;
        int32_t err = make(plug, "OMX.bogus.no.such.codec", &cb, NULL, &comp);
        printf("PROBE plugin name=OMX.bogus.no.such.codec err=0x%08x(%s) comp=%p (bogus control)\n",
                (uint32_t)err, omx_err_name(err), (void *)comp);
        if (err == 0 && comp != NULL && destroy != NULL) {
            destroy(plug, comp);
        }
    }
    {
        OMX_COMPONENTTYPE *comp = NULL;
        int32_t err = make(plug, "OMX.google.mp3.decoder", &cb, NULL, &comp);
        printf("PROBE plugin name=OMX.google.mp3.decoder err=0x%08x(%s) comp=%p (control)\n",
                (uint32_t)err, omx_err_name(err), (void *)comp);
        if (err == 0 && comp != NULL && destroy != NULL) {
            destroy(plug, comp);
        }
    }
    {
        // Adapter base for vtable file-vaddr computation (destroy-cave).
        void *ah = dlopen("libstagefright_soft_s_ac3.so", RTLD_NOW);
        if (ah != NULL) {
            void *fsym = dlsym(ah,
                    "_Z22createSoftOMXComponentPKcPK16OMX_CALLBACKTYPEPvPP17OMX_COMPONENTTYPE");
            Dl_info ai;
            memset(&ai, 0, sizeof(ai));
            if (fsym != NULL && dladdr(fsym, &ai) != 0) {
                printf("PROBE adapter fbase=0x%08lx saddr=0x%08lx\n",
                        (unsigned long)(uintptr_t)ai.dli_fbase,
                        (unsigned long)(uintptr_t)ai.dli_saddr);
            }
            dlclose(ah);
        }
    }
    if (0) {
        // (E) DISABLED (custom PortDef struct risk). Reference: query STOCK MS decoders' input PortDef eEncoding
        // + role, to mimic exactly what MTK ACodec expects per MIME.
        static const char *kRefNames[2] = {
            "OMX.MS.AC3.Decoder", "OMX.MS.DTS.Decoder",
        };
        for (int i = 0; i < 2; i++) {
            OMX_COMPONENTTYPE *comp = NULL;
            int32_t err = make(plug, kRefNames[i], &cb, NULL, &comp);
            printf("PROBE ref name=%s err=0x%08x(%s) comp=%p\n",
                    kRefNames[i], (uint32_t)err, omx_err_name(err),
                    (void *)comp);
            if (err == 0 && comp != NULL) {
                struct {
                    uint32_t nSize;
                    uint8_t nVersion[4];
                    uint32_t nPortIndex;
                    uint32_t eDir;
                    uint32_t nNumBuffers;
                    uint32_t nBufferCountActual;
                    uint32_t nBufferCountMin;
                    uint32_t nBufferSize;
                    uint32_t bEnabled;
                    uint32_t bPopulated;
                    uint32_t eDomain;
                    uint8_t format[64];
                } pd;
                memset(&pd, 0, sizeof(pd));
                pd.nSize = sizeof(pd);
                pd.nPortIndex = 0;
                int32_t ge = comp->GetParameter(
                        comp, (OMX_INDEXTYPE)0x2000001, &pd);
                uint32_t enc = 0;
                char *mime = NULL;
                memcpy(&enc, pd.format + 12, 4);
                memcpy(&mime, pd.format + 0, 4);
                printf("PROBE ref name=%s portdef err=0x%08x enc=0x%08x mime=%s\n",
                        kRefNames[i], (uint32_t)ge, enc,
                        mime ? mime : "?");
                destroy(plug, comp);
            }
        }
    }
    if (0) {
        // (F) ACodec-sequence replay (in-probe, deterministic stdout):
        // TEMPORARILY NEUTERED (configure bring-up): its in-process
        // parameter calls interleave with the service-side ACodec logs
        // and its hand-built sizes pollute failure analysis. Re-enable
        // after configure=0.
        // drive our ac3 adapter through EXACTLY the calls ACodec makes
        // (Role, PortDef GET/SET, AudioInit GET/SET, MTK GET, PortDef GET,
        // Ac3-struct, Pcm) and print EVERY result. Definitive verdict on
        // component-side acceptance (no logd/chatty dependence).
        OMX_COMPONENTTYPE *comp = NULL;
        int32_t err = make(plug, "OMX.test.spatial.ac3", &cb, NULL, &comp);
        printf("PROBE replay make err=0x%08x comp=%p\n",
                (uint32_t)err, (void *)comp);
        if (err == 0 && comp != NULL) {
            static const uint32_t kIdx[] = {
                0x1000017, 0x2000001, 0x2000001, 0x4000002, 0x4000002,
                0x6F400004, 0x6F400009, 0x2000001, 0x6F400001, 0x4000001,
            };
            static const unsigned kSize[] = {
                136, 64, 64, 16, 16, 20, 64, 64, 20, 104,
            };
            static const char *kTag[] = {
                "Role-S", "PortDef-G", "PortDef-S", "AudioInit-G",
                "AudioInit-S", "MTK4-G", "MTK9-G", "PortDef-G2",
                "Ac3?", "Pcm?",
            };
            for (unsigned k = 0; k < sizeof(kIdx) / sizeof(kIdx[0]); k++) {
                uint8_t buf[256];
                memset(buf, 0, sizeof(buf));
                ((uint32_t *)buf)[0] = kSize[k];
                ((uint32_t *)buf)[2] = 0;
                if (kIdx[k] == 0x1000017) {
                    strcpy((char *)buf + 8, "audio_decoder.ac3");
                }
                int32_t g = comp->GetParameter(
                        comp, (OMX_INDEXTYPE)kIdx[k], buf);
                int32_t s = comp->SetParameter(
                        comp, (OMX_INDEXTYPE)kIdx[k], buf);
                printf("PROBE replay %s idx=0x%08x get=0x%08x set=0x%08x\n",
                        kTag[k], kIdx[k], (uint32_t)g, (uint32_t)s);
            }
            destroy(plug, comp);
        }
    }
    free(plug);
    if (0) {
        // (D) OMXMaster-level enumeration sweep: DISABLED (mctor instability). reveals global component
        // order across ALL registered plugins (which names at which global
        // index). Tells us SoftOMXPlugin's position for SOFTIDX.
        typedef void *(*MasterCtorFn)(void *);
        typedef int32_t (*MasterEnumFn)(void *, char *, unsigned, unsigned);
        MasterCtorFn mctor = (MasterCtorFn)dlsym(
                h, "_ZN7android9OMXMasterC1Ev");
        MasterEnumFn menum = (MasterEnumFn)dlsym(
                h, "_ZN7android9OMXMaster19enumerateComponentsEPcjj");
        printf("PROBE master ctor=%p enum=%p\n",
                (void *)mctor, (void *)menum);
        if (mctor != NULL && menum != NULL) {
            void *m = calloc(1, 4096);
            if (m != NULL) {
                mctor(m);
                int consec = 0;
                for (unsigned i = 0; i < 220; i++) {
                    char buf[128];
                    memset(buf, 0, sizeof(buf));
                    int32_t err = menum(m, buf, sizeof(buf), i);
                    if (err == 0) {
                        printf("PROBE enum idx=%u name=%s\n", i, buf);
                        consec = 0;
                    } else {
                        if (consec == 0) {
                            printf("PROBE enum idx=%u err=0x%08x (first miss)\n",
                                    i, (uint32_t)err);
                        }
                        consec++;
                        if (consec > 6) {
                            break;
                        }
                    }
                }
                free(m);
            }
        }
    }
    return 0;
}
