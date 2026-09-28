#pragma once

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace spsc {

// Spin-wait hint: lowers power, yields pipeline resources to the SMT sibling,
// and avoids the memory-order-violation flush when the spin loop exits.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

}  // namespace spsc
