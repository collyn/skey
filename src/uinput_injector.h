#ifndef SKEY_UINPUT_INJECTOR_H
#define SKEY_UINPUT_INJECTOR_H

#include "injection_request.h"
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
    using Request = InjectionRequest;
    using State = Request::State;
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
    // The warmup must outlast the compositor's device-registration window:
    // mutter (GNOME 50.1, Ubuntu 26.04) silently drops keys injected into a
    // uinput device it has not finished registering — measured 0.3s fails,
    // 4s delivers (the old 1s default raced the window: "aà", "chaào" right
    // after fcitx5 restarts/reloads, while minutes-old devices worked).
    explicit UinputInjector(std::unique_ptr<Device> device,
                            std::chrono::milliseconds warmup = std::chrono::seconds(4));
    ~UinputInjector();
    void start();
    bool ready() const;
    std::string status() const;
    // Acceptance means queued, not that the application has deleted text.
    // A nonempty queue is bounded and serialized; requests are never replaced.
    Ticket backspaces(Owner owner, int count, bool escape, unsigned paceUsec);
    // Shift-select replacement: Shift down + Left x(count + 1) + Shift up.
    // The engine consumes the trailing (anchor) Left on its loopback, so the
    // app selects exactly count characters; the commit then replaces the
    // selection.  For fields where Backspace is eaten (browser address bars).
    Ticket shiftSelect(Owner owner, int count, unsigned paceUsec);
    void cancel(Owner owner);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
