// Host shim definitions for Android-only symbols used by
// property_bridge.c. Compiled into host test binaries ONLY (never into
// device builds). Property reads are backed by environment variables so
// tests can control them with setenv/unsetenv.
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

int __system_property_get(const char *key, char *value) {
    if (key == NULL || value == NULL) {
        return 0;
    }
    const char *v = getenv(key);
    if (v == NULL || *v == '\0') {
        return 0;
    }
    size_t n = strlen(v);
    if (n >= 92) {
        n = 91;
    }
    memcpy(value, v, n);
    value[n] = '\0';
    return (int)n;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;
    (void)tag;
    (void)fmt;
    return 0;
}
