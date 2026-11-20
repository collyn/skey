#ifndef SKEY_BACKGROUND_SCAN_H
#define SKEY_BACKGROUND_SCAN_H

// Runs program-wide /proc scans off the input thread.
//
// Identifying an unknown app (Electron? Firefox fork? snap? WebKitGTK? a
// terminal hosting a shell?) walks every process on the system: 20-55ms on
// an idle desktop, 80-140ms under CPU load (measured 2026-11-18).  On the
// fcitx5 main loop that delayed the first keys after every focus change.
// The engine asks take(); an unknown answer is queued here and reported as
// pending, and the engine re-evaluates when generation() moves.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace skey {

class BackgroundScanner {
public:
  /// Runs one scan: (trait, program) → verdict.  Must be thread-safe.
  using Runner = std::function<bool(char trait, const std::string &program)>;

  /// `synchronous`: take() scans inline (tests, engines without a desktop).
  BackgroundScanner(Runner runner, bool synchronous)
      : runner_(std::move(runner)), synchronous_(synchronous) {}

  ~BackgroundScanner() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  BackgroundScanner(const BackgroundScanner &) = delete;
  BackgroundScanner &operator=(const BackgroundScanner &) = delete;

  /// The verdict once known — handed over once, the caller caches it.
  /// Unknown: queues the scan (once) and returns nullopt.
  std::optional<bool> take(char trait, const std::string &program) {
    if (synchronous_) return runner_(trait, program);
    const std::string key = std::string(1, trait) + program;
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = results_.find(key); it != results_.end()) {
      const bool value = it->second;
      results_.erase(it);
      return value;
    }
    if (queued_.insert(key).second) {
      if (queue_.size() >= kMaxQueue) {
        queued_.erase(queue_.front());
        queue_.pop_front();
      }
      queue_.push_back(key);
      if (!worker_.joinable()) worker_ = std::thread(&BackgroundScanner::run, this);
      cv_.notify_one();
    }
    return std::nullopt;
  }

  /// Bumped after each finished scan.
  uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

  /// True while scans are queued or running (tests and diagnostics).
  bool busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !queued_.empty();
  }

private:
  static constexpr size_t kMaxQueue = 64;
  static constexpr size_t kMaxResults = 256;

  void run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      const std::string key = std::move(queue_.front());
      queue_.pop_front();
      lock.unlock();
      const bool value = runner_(key[0], key.substr(1));
      lock.lock();
      queued_.erase(key);
      if (results_.size() >= kMaxResults) results_.clear();
      results_[key] = value;
      generation_.fetch_add(1, std::memory_order_acq_rel);
    }
  }

  Runner runner_;
  const bool synchronous_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  std::unordered_set<std::string> queued_;
  std::unordered_map<std::string, bool> results_;
  std::atomic<uint64_t> generation_{0};
  bool stopping_ = false;
  std::thread worker_;
};

} // namespace skey

#endif
