#ifndef SKEY_LIBEI_INJECTOR_H
#define SKEY_LIBEI_INJECTOR_H
#include <memory>
#include <string>
namespace skey {
// One keyboard-only portal session per engine. No portal or EI work on the
// Fcitx key thread. A refused session never falls back to /dev/uinput.
class LibeiInjector {
public:
    LibeiInjector();
    ~LibeiInjector();
    void start();
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
