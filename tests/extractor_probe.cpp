// extractor_probe.cpp — production-feed ground truth (device-side).
//
// Uses the NDK AMediaExtractor (same stagefright extractors production
// MediaCodec feeds from) to dump what a container yields: track list,
// audio MIME, then per-sample index/size/pts/flags plus a syncword check
// on every sample payload (proves frame alignment of the real feed).
//
// NOTE: compiled as C++ only to work around an NDK r21 header bug
// (NdkMediaCodec.h uses AMediaCodecOnAsyncNotifyCallback without a struct
// tag, which C rejects); the code itself is plain C-style.
//
// Usage:
//   extractor_probe <media.mp4>
// Exit 0 with PROBE summary lines on stdout (unbuffered).

#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    if (argc < 2) {
        printf("PROBE usage: extractor_probe <media>\n");
        return 2;
    }

    AMediaExtractor *ex = AMediaExtractor_new();
    if (!ex) {
        printf("PROBE FAIL extractor_new\n");
        return 1;
    }
    // NOTE: setDataSource(path) routes through Java and needs a JVM
    // (getJNIEnv NULL-crash in a native process). The FD variant is the
    // pure-native path.
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("PROBE FAIL open %s\n", argv[1]);
        AMediaExtractor_delete(ex);
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        printf("PROBE FAIL fstat %s\n", argv[1]);
        close(fd);
        AMediaExtractor_delete(ex);
        return 1;
    }
    if (AMediaExtractor_setDataSourceFd(ex, fd, 0, (off64_t)st.st_size)
            != AMEDIA_OK) {
        printf("PROBE FAIL setDataSourceFd %s\n", argv[1]);
        close(fd);
        AMediaExtractor_delete(ex);
        return 1;
    }
    close(fd);

    size_t ntracks = AMediaExtractor_getTrackCount(ex);
    printf("PROBE tracks=%zu\n", ntracks);
    int audioIdx = -1;
    for (size_t i = 0; i < ntracks; i++) {
        AMediaFormat *fmt = AMediaExtractor_getTrackFormat(ex, i);
        const char *mime = NULL;
        if (fmt) {
            AMediaFormat_getString(fmt, "mime", &mime);
        }
        printf("PROBE track=%zu mime=%s\n", i, mime ? mime : "?");
        if (mime && strncmp(mime, "audio/", 6) == 0 && audioIdx < 0) {
            audioIdx = (int)i;
        }
        if (fmt) {
            AMediaFormat_delete(fmt);
        }
    }
    if (audioIdx < 0) {
        printf("PROBE FAIL no-audio-track\n");
        AMediaExtractor_delete(ex);
        return 1;
    }
    AMediaExtractor_selectTrack(ex, (size_t)audioIdx);

    uint8_t *buf = (uint8_t *)malloc(256 * 1024);
    if (!buf) {
        printf("PROBE FAIL nomem\n");
        AMediaExtractor_delete(ex);
        return 1;
    }
    long count = 0;
    long long totalBytes = 0;
    long minSize = -1, maxSize = 0;
    long syncHits = 0;
    long syncFails = 0;
    for (;;) {
        int idx = AMediaExtractor_getSampleTrackIndex(ex);
        if (idx < 0) {
            break;
        }
        ssize_t size = AMediaExtractor_getSampleSize(ex);
        int64_t t = AMediaExtractor_getSampleTime(ex);
        uint32_t flags = AMediaExtractor_getSampleFlags(ex);
        ssize_t got = AMediaExtractor_readSampleData(ex, buf, 256 * 1024);
        if (count < 5 || (count % 50) == 0) {
            printf("PROBE sample=%ld size=%zd read=%zd pts=%lld flags=%u head=%02x%02x\n",
                    count, size, got, (long long)t, flags,
                    got > 0 ? buf[0] : 0, got > 1 ? buf[1] : 0);
        }
        if (got >= 2) {
            if (buf[0] == 0x0B && buf[1] == 0x77) {
                syncHits++;
            } else {
                syncFails++;
                if (syncFails <= 5) {
                    printf("PROBE nosync sample=%ld head=%02x%02x\n",
                            count, buf[0], buf[1]);
                }
            }
        }
        count++;
        totalBytes += (long long)got;
        if (minSize < 0 || got < minSize) {
            minSize = (long)got;
        }
        if (got > maxSize) {
            maxSize = (long)got;
        }
        if (!AMediaExtractor_advance(ex)) {
            break;
        }
    }
    printf("PROBE summary samples=%ld bytes=%lld min=%ld max=%ld synchits=%ld syncfails=%ld\n",
            count, totalBytes, minSize, maxSize, syncHits, syncFails);
    free(buf);
    AMediaExtractor_delete(ex);
    return 0;
}
