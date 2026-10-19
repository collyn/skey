#ifndef SKEY_UINPUT_INJECTOR_H
#define SKEY_UINPUT_INJECTOR_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>

namespace skey {
// One keyboard per engine. Only the worker touches the device; it never
// accesses fcitx, an InputContext, or composition state.
class UinputInjector {
public:
    using Owner = uint64_t;
    enum class State { Queued, Complete, Failed, Cancelled };
    struct Request {
        std::atomic<State> state{State::Queued};
    };
    using Ticket = std::shared_ptr<Request>;

    // Device boundary for isolated tests. open() includes configuration;
    // write() follows write(2), including errno and event-aligned short writes.
    class Device {
    public:
        virtual ~Device() = default;
        virtual bool open(std::string &error) = 0;
        virtual ssize_t write(const void *data, size_t size) = 0;
        virtual void close() = 0;
    };
    UinputInjector();
    explicit UinputInjector(std::unique_ptr<Device> device,
                            std::chrono::milliseconds warmup = std::chrono::seconds(1));
    ~UinputInjector();
    void start();
    bool ready() const;
    std::string status() const;
    // Acceptance means queued, not that the application has deleted text.
    // A nonempty queue is bounded and serialized; requests are never replaced.
    Ticket backspaces(Owner owner, int count, bool escape, unsigned paceUsec);
    void cancel(Owner owner);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
