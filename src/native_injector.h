#ifndef SKEY_NATIVE_INJECTOR_H
#define SKEY_NATIVE_INJECTOR_H
#include <memory>
#include <string>
namespace skey {
// XTest on X11; a keyboard-only Libei portal session on Wayland.
// Portal/EI work runs on a worker. Neither transport falls back to Uinput.
class NativeInjector {
public:
    NativeInjector();
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
