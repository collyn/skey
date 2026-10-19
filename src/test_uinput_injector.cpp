#include "uinput_injector.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <linux/input.h>
#include <mutex>
#include <thread>
#include <vector>

using Injector = skey::UinputInjector;
using namespace std::chrono_literals;
static void check(bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
template<class F> static void until(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        check(std::chrono::steady_clock::now() < deadline, "worker deadline");
        std::this_thread::sleep_for(1ms);
    }
}
struct Data {
    std::mutex mutex;
    std::vector<input_event> events;
    std::vector<std::chrono::steady_clock::time_point> taps;
    std::deque<int> writes; // negative errno, zero write, or event count
    int opens = 0, closes = 0, failOpens = 0;
};
class FakeDevice : public Injector::Device {
public:
    explicit FakeDevice(std::shared_ptr<Data> d) : data(std::move(d)) {}
    bool open(std::string &error) override {
        std::lock_guard<std::mutex> lock(data->mutex);
        ++data->opens;
        if (data->failOpens-- > 0) { error = "permission denied"; return false; }
        return true;
    }
    ssize_t write(const void *bytes, size_t size) override {
        std::lock_guard<std::mutex> lock(data->mutex);
        if (!data->writes.empty()) {
            const int result = data->writes.front(); data->writes.pop_front();
            if (result < 0) { errno = -result; return -1; }
            size = std::min(size, result * sizeof(input_event));
        }
        const auto *events = static_cast<const input_event *>(bytes);
        for (size_t i = 0; i < size / sizeof(input_event); ++i) {
            data->events.push_back(events[i]);
            if (events[i].type == EV_KEY && events[i].value == 1)
                data->taps.push_back(std::chrono::steady_clock::now());
        }
        return size;
    }
    void close() override {
        std::lock_guard<std::mutex> lock(data->mutex);
        ++data->closes;
    }
private:
    std::shared_ptr<Data> data;
};
static void complete(const Injector::Ticket &ticket) {
    check(bool(ticket), "request accepted");
    until([&] { return ticket->state.load() != Injector::State::Queued; });
}
int main() {
    {
        auto data = std::make_shared<Data>();
        Injector injector(std::make_unique<FakeDevice>(data), 0ms);
        check(!injector.backspaces(1, 2, false, 1000), "unstarted must reject");
        injector.start(); injector.start();
        until([&] { return injector.ready(); });
        check(!injector.backspaces(1, 65, false, 1000), "oversized request rejected, never truncated");
        auto first = injector.backspaces(1, 3, true, 5000);
        auto second = injector.backspaces(2, 2, false, 1000);
        complete(first); complete(second);
        check(first->state.load() == Injector::State::Complete &&
              second->state.load() == Injector::State::Complete, "serialized completion");
        std::lock_guard<std::mutex> lock(data->mutex);
        check(data->opens == 1 && data->events.size() == 24, "one device and six taps");
        for (size_t i = 0; i < data->events.size(); i += 4) {
            const auto *e = data->events.data() + i;
            check(e[0].type == EV_KEY && e[0].code == (i == 0 ? KEY_ESC : KEY_BACKSPACE) &&
                  e[0].value == 1 && e[1].type == EV_SYN && e[1].code == SYN_REPORT &&
                  e[2].type == EV_KEY && e[2].code == e[0].code && e[2].value == 0 &&
                  e[3].type == EV_SYN && e[3].code == SYN_REPORT, "tap contract");
        }
        check(data->taps[1] - data->taps[0] >= 4500us &&
              data->taps[2] - data->taps[1] >= 4500us, "requested pacing retained");
    }
    {
        auto data = std::make_shared<Data>();
        Injector injector(std::make_unique<FakeDevice>(data), 0ms);
        injector.start(); until([&] { return injector.ready(); });
        auto old = injector.backspaces(1, 64, false, 100000);
        until([&] { std::lock_guard<std::mutex> l(data->mutex); return !data->events.empty(); });
        auto fresh = injector.backspaces(2, 2, false, 0);
        injector.cancel(1); injector.cancel(1);
        complete(fresh);
        check(old->state.load() == Injector::State::Cancelled &&
              fresh->state.load() == Injector::State::Complete, "old owner cannot cancel new owner");
        auto busy = injector.backspaces(3, 64, false, 100000);
        until([&] { std::lock_guard<std::mutex> l(data->mutex); return data->events.size() >= 16; });
        std::vector<Injector::Ticket> queued;
        for (int i = 0; i < 8; ++i) queued.push_back(injector.backspaces(4, 1, false, 0));
        check(!injector.backspaces(5, 1, false, 0), "full queue rejects without overwriting");
        injector.cancel(4); injector.cancel(3);
        for (auto &t : queued) check(t && t->state.load() == Injector::State::Cancelled, "queued cancellation");
    }
    {
        auto data = std::make_shared<Data>();
        data->writes = {-EINTR, 2, -EAGAIN, 2};
        Injector injector(std::make_unique<FakeDevice>(data), 0ms);
        injector.start(); until([&] { return injector.ready(); });
        auto t = injector.backspaces(1, 2, false, 1000); complete(t);
        check(t->state.load() == Injector::State::Complete, "aligned partial writes retried");
        std::lock_guard<std::mutex> l(data->mutex);
        check(data->events.size() == 8, "partial write does not duplicate events");
    }
    for (bool partial : {false, true}) {
        auto data = std::make_shared<Data>();
        if (partial) data->writes.push_back(2);
        data->writes.push_back(-EIO);
        Injector injector(std::make_unique<FakeDevice>(data), 0ms);
        injector.start(); until([&] { return injector.ready(); });
        auto t = injector.backspaces(1, 3, false, 1000); complete(t);
        check(t->state.load() == Injector::State::Failed && !injector.ready(), "write failure reported");
        until([&] { return injector.ready(); });
        std::lock_guard<std::mutex> l(data->mutex);
        check(data->opens == 2 && data->events.size() == (partial ? 2u : 0u), "failed batch never replayed after retry");
    }
    {
        auto data = std::make_shared<Data>(); data->failOpens = 1;
        Injector injector(std::make_unique<FakeDevice>(data), 0ms);
        injector.start(); until([&] { return injector.ready(); });
        std::lock_guard<std::mutex> l(data->mutex);
        check(data->opens == 2, "late permission/device retries");
    }
    for (bool backpressure : {false, true}) {
        auto data = std::make_shared<Data>();
        data->writes = backpressure ? std::deque<int>(21, -EAGAIN) : std::deque<int>{0};
        Injector injector(std::make_unique<FakeDevice>(data), 0ms);
        injector.start(); until([&] { return injector.ready(); });
        auto t = injector.backspaces(1, 2, false, 1000); complete(t);
        check(t->state.load() == Injector::State::Failed, "zero writes and sustained backpressure fail boundedly");
    }
    {
        auto data = std::make_shared<Data>();
        Injector::Ticket pending;
        const auto start = std::chrono::steady_clock::now();
        {
            Injector injector(std::make_unique<FakeDevice>(data), 0ms);
            injector.start(); until([&] { return injector.ready(); });
            pending = injector.backspaces(1, 64, false, 100000);
            until([&] { std::lock_guard<std::mutex> l(data->mutex); return !data->events.empty(); });
        }
        check(pending->state.load() == Injector::State::Cancelled &&
              std::chrono::steady_clock::now() - start < 500ms, "shutdown interrupts pacing and cancels remaining keys");
    }
    const auto start = std::chrono::steady_clock::now();
    {
        auto data = std::make_shared<Data>();
        Injector injector(std::make_unique<FakeDevice>(data), 10s);
        injector.start();
        until([&] { std::lock_guard<std::mutex> l(data->mutex); return data->opens == 1; });
        check(!injector.ready() && !injector.backspaces(1, 2, false, 1000), "warmup rejects safely");
    }
    check(std::chrono::steady_clock::now() - start < 500ms, "shutdown interrupts warmup");
    std::cout << "Uinput worker tests passed\n";
}
