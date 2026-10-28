#ifndef SKEY_INJECTION_REQUEST_H
#define SKEY_INJECTION_REQUEST_H
#include <atomic>
#include <cstdint>
namespace skey {
struct InjectionRequest {
    enum class State { Queued, Running, Complete, Failed, Cancelled };
    std::atomic<State> state{State::Queued};
    // Conservative transport progress: includes attempted writes whose delivery
    // is uncertain. Zero alone is safe for fallback only after a terminal state.
    std::atomic<uint64_t> emittedEvents{0};
    uint64_t id = 0;
};
}
#endif
