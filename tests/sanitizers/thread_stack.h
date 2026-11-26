// Force-included (-include) into every C++ translation unit of a sanitizer
// build by scripts/sanitize.py; never included by the sources themselves and
// never part of the normal or the guest build.
//
// The server sizes its thread stacks explicitly (128 to 512 KiB, the guest's
// memory budget). ASan's redzones and TSan's instrumentation make every frame
// several times larger, so the owner threads overflow those stacks in a
// sanitizer build although the uninstrumented code fits. Sanitizer builds
// therefore get eight times the requested stack. They say nothing about
// whether the production sizes (256 KiB) are sufficient: the normal host
// build runs the same tests with the real sizes, and the guest boot smoke
// runs them in the guest. Threads started without an explicit size
// (std::thread, default attributes) keep the platform default.
#ifndef FORTIS_SANITIZER_THREAD_STACK_H
#define FORTIS_SANITIZER_THREAD_STACK_H

#include <pthread.h>
#include <stddef.h>

static inline int fortis_sanitizer_setstacksize(pthread_attr_t* attributes, size_t bytes) {
  return pthread_attr_setstacksize(attributes, bytes * 8);
}
#define pthread_attr_setstacksize fortis_sanitizer_setstacksize

#endif
