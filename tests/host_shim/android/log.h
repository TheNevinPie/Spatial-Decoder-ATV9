// Host shim for <android/log.h>.
// Discards log output on non-Android hosts. On device builds the real
// NDK header is used instead (see CMakeLists).
#ifndef HOST_SHIM_ANDROID_LOG_H
#define HOST_SHIM_ANDROID_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HOST_SHIM_ANDROID_LOG_WARN = 5
};
#define ANDROID_LOG_WARN HOST_SHIM_ANDROID_LOG_WARN

int __android_log_print(int prio, const char *tag, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
