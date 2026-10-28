#ifndef SKEY_NATIVE_INJECTOR_H
#define SKEY_NATIVE_INJECTOR_H
#include "injection_request.h"
#include <memory>
#include <string>
namespace skey {
// XTest on X11; a keyboard-only Libei portal session on Wayland.
// Both connections and pacing run on a worker. Neither falls back to Uinput.
class NativeInjector {
public:
    using Owner = uint64_t;
    using Request = InjectionRequest;
    using State = Request::State;
    using Ticket = std::shared_ptr<Request>;
    class Transport {
    public:
        virtual ~Transport() = default;
        virtual bool open(std::string &error) = 0;
        virtual unsigned keycode(unsigned long keysym) = 0;
        virtual bool modifiers(unsigned &mask) = 0;
        virtual bool focusWindow(uint64_t &window) { window = 1; return true; }
        virtual bool key(unsigned code, bool down) = 0;
        virtual bool flush() = 0;
        virtual void close() = 0;
    };
    NativeInjector();
    explicit NativeInjector(std::unique_ptr<Transport> transport);
    // X11-only ticket API. A null ticket means not accepted. Complete means
    // submitted to the X server, not acknowledged by the focused application.
    // Pin expectedWindow when submitting from an input context; zero captures
    // worker-time focus for standalone callers without a known target.
    Ticket submitBackspaces(Owner owner, int count, bool escape, unsigned paceUsec,
                            uint64_t expectedWindow = 0);
    Ticket submitShiftSelect(Owner owner, int count, unsigned paceUsec,
                             uint64_t expectedWindow = 0);
    void cancel(Owner owner);
    ~NativeInjector();
    void start();
    // Restore an already-authorized Plasma portal session after daemon restart.
    // This initializes connectivity only; output routing remains unchanged.
    void restoreSession();
    bool ready() const;
    std::string status() const;
    bool backspaces(int count, bool escape, unsigned paceUsec);
    void cancel();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
