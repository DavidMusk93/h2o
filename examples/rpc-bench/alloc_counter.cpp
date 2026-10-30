#include "benchmark.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>

extern "C" {
void *__libc_malloc(std::size_t size);
void *__libc_calloc(std::size_t count, std::size_t size);
void *__libc_realloc(void *pointer, std::size_t size);
void *__libc_memalign(std::size_t alignment, std::size_t size);
void __libc_free(void *pointer);
}

namespace {
std::atomic<std::uint64_t> g_allocation_count{0};

void countAllocation() {
  g_allocation_count.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

extern "C" void *malloc(std::size_t size) {
  countAllocation();
  return __libc_malloc(size);
}

extern "C" void *calloc(std::size_t count, std::size_t size) {
  countAllocation();
  return __libc_calloc(count, size);
}

extern "C" void *realloc(void *pointer, std::size_t size) {
  countAllocation();
  return __libc_realloc(pointer, size);
}

extern "C" void free(void *pointer) { __libc_free(pointer); }

extern "C" void *aligned_alloc(std::size_t alignment, std::size_t size) {
  countAllocation();
  return __libc_memalign(alignment, size);
}

extern "C" int posix_memalign(void **result, std::size_t alignment,
                              std::size_t size) {
  void *pointer = __libc_memalign(alignment, size);
  if (pointer == nullptr) {
    return ENOMEM;
  }
  countAllocation();
  *result = pointer;
  return 0;
}

namespace flowkit::auto_bench::rpc {

std::uint64_t allocationCount() {
  return g_allocation_count.load(std::memory_order_relaxed);
}

} // namespace flowkit::auto_bench::rpc
