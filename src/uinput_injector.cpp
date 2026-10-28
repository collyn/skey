#include "uinput_injector.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <mutex>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace skey {
namespace {
class KernelDevice final : public UinputInjector::Device {
public:
    ~KernelDevice() override { close(); }
    bool open(std::string &error) override {
        fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) return fail(error, "open /dev/uinput");
        if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0 ||
            ioctl(fd_, UI_SET_KEYBIT, KEY_BACKSPACE) < 0 ||
            ioctl(fd_, UI_SET_KEYBIT, KEY_ESC) < 0 ||
            ioctl(fd_, UI_SET_KEYBIT, KEY_LEFTSHIFT) < 0 ||
            ioctl(fd_, UI_SET_KEYBIT, KEY_LEFT) < 0)
            return fail(error, "configure /dev/uinput");
        uinput_setup setup{};
        setup.id.bustype = BUS_USB;
        setup.id.vendor = 0x534b;
        setup.id.product = 1;
        std::strncpy(setup.name, "SKey-Uinput", UINPUT_MAX_NAME_SIZE - 1);
        if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0 || ioctl(fd_, UI_DEV_CREATE) < 0)
            return fail(error, "create uinput keyboard");
        created_ = true;
        return true;
    }
    ssize_t write(const void *data, size_t size) override { return ::write(fd_, data, size); }
    void close() override {
        if (fd_ >= 0) {
            if (created_) ioctl(fd_, UI_DEV_DESTROY);
            ::close(fd_);
        }
        fd_ = -1;
        created_ = false;
    }
private:
    bool fail(std::string &error, const char *operation) {
        error = std::string(operation) + ": " + std::strerror(errno);
        close();
        return false;
    }
    int fd_ = -1;
    bool created_ = false;
};
}

struct UinputInjector::Impl {
    struct Batch { Owner owner; int count; bool escape; bool shiftSelect; unsigned pace; Ticket ticket; };
    std::unique_ptr<Device> device;
    std::chrono::milliseconds warmup;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool stopping = false, available = false;
    std::string message = "not started";
    std::deque<Batch> queue;
    Ticket active;
    Owner activeOwner = 0;

    Impl(std::unique_ptr<Device> d, std::chrono::milliseconds w)
        : device(std::move(d)), warmup(w) {}
    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
    bool interrupted(const Ticket &ticket) const {
        return stopping || (ticket && ticket->state.load() == State::Cancelled);
    }
    // mutex held. Waiting always releases it, including startup and pacing.
    bool pause(std::unique_lock<std::mutex> &lock, std::chrono::microseconds delay,
               const Ticket &ticket = {}) {
        return wake.wait_for(lock, delay, [&] { return interrupted(ticket); });
    }
    void closeDevice(std::unique_lock<std::mutex> &lock) {
        lock.unlock();
        device->close();
        lock.lock();
    }
    // Write the events with bounded backpressure retries.  A cancelled batch
    // still completes its writes, so no pressed key remains stuck.
    bool send(std::unique_lock<std::mutex> &lock, const input_event *events,
              size_t count, const Ticket &ticket) {
        size_t offset = 0;
        int retries = 0;
        const size_t bytes = count * sizeof(input_event);
        while (offset < bytes) {
            lock.unlock();
            const auto n = device->write(reinterpret_cast<const char *>(events) + offset,
                                         bytes - offset);
            const int writeError = errno;
            lock.lock();
            if (n > 0 && static_cast<size_t>(n) <= bytes - offset &&
                n % sizeof(input_event) == 0) {
                offset += n;
                retries = 0;
                continue;
            }
            if (n < 0 && (writeError == EINTR || writeError == EAGAIN) && ++retries <= 20) {
                // Bound device backpressure; a cancelled partial tap is still
                // released, or the failed device is destroyed by run().
                wake.wait_for(lock, std::chrono::milliseconds(1));
                continue;
            }
            message = n < 0 ? std::string("write /dev/uinput: ") + std::strerror(writeError)
                            : "write /dev/uinput: invalid short write";
            ticket->state.store(State::Failed);
            return false;
        }
        return true;
    }
    bool tap(std::unique_lock<std::mutex> &lock, int code, const Ticket &ticket) {
        input_event events[4]{};
        events[0].type = events[2].type = EV_KEY;
        events[0].code = events[2].code = code;
        events[0].value = 1;
        events[1].type = events[3].type = EV_SYN;
        events[1].code = events[3].code = SYN_REPORT;
        return send(lock, events, 4, ticket);
    }
    bool press(std::unique_lock<std::mutex> &lock, int code, const Ticket &ticket) {
        input_event events[2]{};
        events[0].type = EV_KEY;
        events[0].code = code;
        events[0].value = 1;
        events[1].type = EV_SYN;
        events[1].code = SYN_REPORT;
        return send(lock, events, 2, ticket);
    }
    bool release(std::unique_lock<std::mutex> &lock, int code, const Ticket &ticket) {
        input_event events[2]{};
        events[0].type = EV_KEY;
        events[0].code = code;
        events[0].value = 0;
        events[1].type = EV_SYN;
        events[1].code = SYN_REPORT;
        return send(lock, events, 2, ticket);
    }
    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            if (!available) {
                message = "initializing /dev/uinput";
                std::string error;
                lock.unlock();
                const bool opened = device->open(error);
                lock.lock();
                if (!opened) {
                    message = error;
                    closeDevice(lock);
                    if (pause(lock, std::chrono::seconds(1))) break;
                    continue;
                }
                // Preserve the server's compositor discovery interval, off the
                // fcitx thread. Start early when the addon is loaded.
                if (pause(lock, warmup)) break;
                available = true;
                message = "ready";
            }
            wake.wait(lock, [&] { return stopping || !queue.empty(); });
            if (stopping) break;
            auto batch = queue.front();
            queue.pop_front();
            active = batch.ticket;
            activeOwner = batch.owner;
            bool ok = true;
            if (!interrupted(active) && batch.escape) {
                ok = tap(lock, KEY_ESC, active);
                if (ok) pause(lock, std::chrono::microseconds(batch.pace), active);
            }
            // Shift-select replacement: Shift down, Left x(count + 1), Shift
            // up.  The app receives count Lefts (selecting the old suffix);
            // the trailing Left is the confirmation anchor the engine
            // consumes on its loopback.  Used where Backspace is eaten
            // (browser address bars' autocomplete, GNOME omnibox).
            if (ok && batch.shiftSelect) {
                ok = press(lock, KEY_LEFTSHIFT, active);
                for (int i = 0; ok && i < batch.count && !interrupted(active); ++i) {
                    ok = tap(lock, KEY_LEFT, active);
                    if (ok) pause(lock, std::chrono::microseconds(batch.pace), active);
                }
                // Release the Shift BEFORE the anchor so a fast next key
                // can never observe the modifier still down ("chO").
                if (ok && !interrupted(active)) {
                    ok = release(lock, KEY_LEFTSHIFT, active);
                }
                if (ok && !interrupted(active)) {
                    ok = tap(lock, KEY_LEFT, active);
                }
            } else {
                for (int i = 0; ok && i < batch.count && !interrupted(active); ++i) {
                    ok = tap(lock, KEY_BACKSPACE, active);
                    if (ok && i + 1 < batch.count)
                        pause(lock, std::chrono::microseconds(batch.pace), active);
                }
            }
            if (!ok) {
                available = false;
                closeDevice(lock);
                for (auto &pending : queue) pending.ticket->state.store(State::Failed);
                queue.clear();
            } else if (active->state.load() != State::Cancelled) {
                active->state.store(stopping ? State::Cancelled : State::Complete);
            }
            active.reset();
            if (!ok && pause(lock, std::chrono::seconds(1))) break;
        }
        available = false;
        if (active) active->state.store(State::Cancelled);
        for (auto &batch : queue) batch.ticket->state.store(State::Cancelled);
        queue.clear();
        closeDevice(lock);
    }
};

UinputInjector::UinputInjector() : UinputInjector(std::make_unique<KernelDevice>()) {}
UinputInjector::UinputInjector(std::unique_ptr<Device> device, std::chrono::milliseconds warmup)
    : impl_(std::make_unique<Impl>(std::move(device), warmup)) {}
UinputInjector::~UinputInjector() = default;
void UinputInjector::start() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->worker.joinable()) {
        auto *impl = impl_.get();
        impl_->worker = std::thread([impl] { impl->run(); });
    }
}
bool UinputInjector::ready() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->available;
}
std::string UinputInjector::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->message;
}
UinputInjector::Ticket UinputInjector::backspaces(Owner owner, int count, bool escape,
                                                 unsigned paceUsec) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->available || impl_->stopping || count < 1 || count > 64 || impl_->queue.size() >= 8)
        return {};
    auto ticket = std::make_shared<Request>();
    // The old server interpreted zero as its 1ms default. Keep that behavior
    // (including manual zero overrides) throughout the transport migration.
    const unsigned pace = paceUsec == 0 ? 1000 : std::min(paceUsec, 100000u);
    impl_->queue.push_back({owner, count, escape, false, pace, ticket});
    impl_->wake.notify_all();
    return ticket;
}

UinputInjector::Ticket UinputInjector::shiftSelect(Owner owner, int count,
                                                   unsigned paceUsec) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->available || impl_->stopping || count < 1 || count > 64 || impl_->queue.size() >= 8)
        return {};
    auto ticket = std::make_shared<Request>();
    const unsigned pace = paceUsec == 0 ? 5000 : std::min(paceUsec, 100000u);
    impl_->queue.push_back({owner, count, false, true, pace, ticket});
    impl_->wake.notify_all();
    return ticket;
}
void UinputInjector::cancel(Owner owner) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->active && impl_->activeOwner == owner) impl_->active->state.store(State::Cancelled);
    for (auto it = impl_->queue.begin(); it != impl_->queue.end();) {
        if (it->owner == owner) {
            it->ticket->state.store(State::Cancelled);
            it = impl_->queue.erase(it);
        } else ++it;
    }
    impl_->wake.notify_all();
}
}
