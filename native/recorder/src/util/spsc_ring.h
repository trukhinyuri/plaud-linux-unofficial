// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Wait-free single-producer/single-consumer ring of float samples. The
// producer is a PipeWire real-time process callback, the consumer is the
// recorder worker thread. No allocation or locking after construction.
//
// Only one thread may consume (Read, Discard, DiscardTo, DiscardAll): a
// second consumer could store an older read index over a newer one and
// replay stale samples. Other threads ask the consumer to drop data instead
// (see WriteIndex()/DiscardTo()). The first consuming thread becomes the
// owner; consumer calls from any other thread are counted in
// consumerViolations() so the engine can report the bug.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

namespace plaud_rec {

class SpscFloatRing {
 public:
  // capacity is rounded up to a power of two.
  explicit SpscFloatRing(size_t capacity) {
    size_t cap = 1;
    while (cap < capacity) cap <<= 1;
    capacity_ = cap;
    mask_ = cap - 1;
    buf_.reset(new float[cap]());
  }

  SpscFloatRing(const SpscFloatRing&) = delete;
  SpscFloatRing& operator=(const SpscFloatRing&) = delete;

  size_t capacity() const { return capacity_; }

  // Producer side. Returns the number of samples stored (less than n when full).
  size_t Write(const float* src, size_t n) {
    const size_t w = write_.load(std::memory_order_relaxed);
    const size_t r = read_.load(std::memory_order_acquire);
    const size_t space = capacity_ - (w - r);
    const size_t count = std::min(n, space);
    const size_t first = std::min(count, capacity_ - (w & mask_));
    if (src != nullptr) {
      std::memcpy(buf_.get() + (w & mask_), src, first * sizeof(float));
      std::memcpy(buf_.get(), src + first, (count - first) * sizeof(float));
    } else {
      std::memset(buf_.get() + (w & mask_), 0, first * sizeof(float));
      std::memset(buf_.get(), 0, (count - first) * sizeof(float));
    }
    write_.store(w + count, std::memory_order_release);
    return count;
  }

  // Producer side: append n zero samples.
  size_t WriteZeros(size_t n) { return Write(nullptr, n); }

  // Any thread: the total number of samples written so far. A snapshot of it
  // marks "everything written up to now" for DiscardTo().
  size_t WriteIndex() const { return write_.load(std::memory_order_acquire); }

  // Consumer side.
  size_t Available() const {
    return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed);
  }

  size_t Read(float* dst, size_t n) {
    CheckConsumer();
    const size_t r = read_.load(std::memory_order_relaxed);
    const size_t w = write_.load(std::memory_order_acquire);
    const size_t count = std::min(n, w - r);
    const size_t first = std::min(count, capacity_ - (r & mask_));
    std::memcpy(dst, buf_.get() + (r & mask_), first * sizeof(float));
    std::memcpy(dst + first, buf_.get(), (count - first) * sizeof(float));
    read_.store(r + count, std::memory_order_release);
    return count;
  }

  size_t Discard(size_t n) {
    CheckConsumer();
    const size_t r = read_.load(std::memory_order_relaxed);
    const size_t w = write_.load(std::memory_order_acquire);
    const size_t count = std::min(n, w - r);
    read_.store(r + count, std::memory_order_release);
    return count;
  }

  // Consumer side: drop everything written before the producer reached
  // `mark` (a WriteIndex() snapshot); data written after it is kept.
  size_t DiscardTo(size_t mark) {
    const size_t r = read_.load(std::memory_order_relaxed);
    if (static_cast<std::ptrdiff_t>(mark - r) <= 0) {
      CheckConsumer();
      return 0;
    }
    return Discard(mark - r);
  }

  // Consumer side: drop everything currently buffered.
  size_t DiscardAll() { return Discard(Available()); }

  uint64_t consumerViolations() const { return consumerViolations_.load(std::memory_order_relaxed); }

 private:
  void CheckConsumer() {
    const std::thread::id self = std::this_thread::get_id();
    std::thread::id owner = consumer_.load(std::memory_order_relaxed);
    if (owner == self) return;
    if (owner == std::thread::id() &&
        consumer_.compare_exchange_strong(owner, self, std::memory_order_relaxed)) {
      return;
    }
    if (owner != self) consumerViolations_.fetch_add(1, std::memory_order_relaxed);
  }

  size_t capacity_ = 0;
  size_t mask_ = 0;
  std::unique_ptr<float[]> buf_;
  alignas(64) std::atomic<size_t> write_{0};
  alignas(64) std::atomic<size_t> read_{0};
  std::atomic<std::thread::id> consumer_{};
  std::atomic<uint64_t> consumerViolations_{0};
};

}  // namespace plaud_rec
