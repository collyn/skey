#ifndef SKEY_INPUT_TIMING_H
#define SKEY_INPUT_TIMING_H
#include <algorithm>
#include <cstdint>

namespace skey {
// Bounds apply only to automatic timing, never to explicit per-app overrides.
inline uint64_t updatedRoundTrip(uint64_t previous, uint64_t sample,
                                uint64_t seed, double alpha) {
    sample = std::min<uint64_t>(sample, 200000);
    if (!previous || previous == seed) return sample;
    // Recover faster after a stall, while retaining half of the historical
    // estimate. Rising latency still uses the existing conservative smoothing.
    if (sample < previous) alpha = std::max(alpha, 0.5);
    return static_cast<uint64_t>(alpha * sample + (1.0 - alpha) * previous);
}

inline uint64_t deferredDelay(uint64_t roundTrip, uint64_t seed) {
    if (!roundTrip || roundTrip == seed) return 15000;
    // Saturate before multiplying, including corrupted/very old samples.
    return std::clamp<uint64_t>(std::min<uint64_t>(roundTrip, 46000) * 2 + 8000,
                               10000, 100000);
}
}
#endif
