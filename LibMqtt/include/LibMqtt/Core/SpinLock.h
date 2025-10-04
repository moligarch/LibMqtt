#pragma once
#include <atomic>
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <immintrin.h>
#endif

namespace libmqtt {

    // Simple spin lock with exponential backoff. Use sparingly.
    // Prefer std::mutex + condition_variable where blocking is needed.
    class SpinLock {
    public:
        void lock() noexcept {
            int spins = 1;
            while (flag_.test_and_set(std::memory_order_acquire)) {
                for (int i = 0; i < spins; ++i) {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
                    _mm_pause();
#endif
                }
                if (spins < (1 << 10)) spins <<= 1;
            }
        }
        bool try_lock() noexcept { return !flag_.test_and_set(std::memory_order_acquire); }
        void unlock() noexcept { flag_.clear(std::memory_order_release); }
    private:
        std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    };

} // namespace libmqtt
