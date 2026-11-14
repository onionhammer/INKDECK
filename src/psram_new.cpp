// C++ allocations (new, std::string, std::vector, std::map nodes) prefer PSRAM.
//
// ESP-IDF's malloc() keeps anything under 4 KB (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, fixed in the
// core's precompiled libraries) in internal RAM. Our C++ makes thousands of small strings: a
// podcast folder of 686 episodes and its play queue took about 150 KB of the board's ~230 KB of
// free internal RAM, and Wi-Fi then couldn't start (its buffers must be internal). Measured with
// dev key 'h' (devtools.h).
//
// Code that needs internal RAM asks for it explicitly with heap_caps_malloc(): DMA buffers (the
// player's input and PCM buffers, I2S, Wi-Fi), task stacks, and anything an IRAM interrupt
// handler touches. Plain C malloc() is unchanged. If PSRAM is full, new falls back to internal.
#include <esp_heap_caps.h>
#include <stdlib.h>

#include <new>

namespace {

void *allocate(size_t size) {
  if (size == 0) size = 1;
  void *p = heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_DEFAULT);
  // The firmware builds without exceptions (-fno-exceptions, platformio.ini), and nothing could
  // catch a std::bad_alloc anyway, so out of memory ends the same way: abort, with a backtrace.
  if (!p) abort();
  return p;
}

void *allocateNoThrow(size_t size) noexcept {
  if (size == 0) size = 1;
  return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

}  // namespace

void *operator new(size_t size) { return allocate(size); }
void *operator new[](size_t size) { return allocate(size); }
void *operator new(size_t size, const std::nothrow_t &) noexcept { return allocateNoThrow(size); }
void *operator new[](size_t size, const std::nothrow_t &) noexcept { return allocateNoThrow(size); }

// heap_caps_free() takes memory from either heap (and from malloc()).
void operator delete(void *p) noexcept { heap_caps_free(p); }
void operator delete[](void *p) noexcept { heap_caps_free(p); }
void operator delete(void *p, size_t) noexcept { heap_caps_free(p); }
void operator delete[](void *p, size_t) noexcept { heap_caps_free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { heap_caps_free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { heap_caps_free(p); }
