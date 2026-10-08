// Host shim for <sys/system_properties.h>.
// Backs __system_property_get with process environment variables so
// property_bridge.c compiles and runs on non-Android hosts. On device
// builds the real NDK header is used instead (see CMakeLists).
#ifndef HOST_SHIM_SYSTEM_PROPERTIES_H
#define HOST_SHIM_SYSTEM_PROPERTIES_H

#ifdef __cplusplus
extern "C" {
#endif

int __system_property_get(const char *key, char *value);

#ifdef __cplusplus
}
#endif

#endif
