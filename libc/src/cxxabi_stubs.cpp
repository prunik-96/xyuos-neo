#include <stdlib.h>
#include <stddef.h>

void *operator new(size_t size) { return malloc(size); }
void *operator new[](size_t size) { return malloc(size); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" void __cxa_pure_virtual() {
    for (;;) { }
}

extern "C" int __cxa_atexit(void (*)(void *), void *, void *) {
    return 0;
}

void *__dso_handle = nullptr;
