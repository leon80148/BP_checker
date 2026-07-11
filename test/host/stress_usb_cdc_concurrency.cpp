#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include "lib/transports/UsbCdcConcurrency.h"
#include "lib/transports/UsbCdcState.h"

template <typename T, size_t Capacity>
class SpscRing {
public:
  bool push(const T& value) {
    size_t head = _head.load(std::memory_order_relaxed);
    size_t next = (head + 1) % Capacity;
    if (next == _tail.load(std::memory_order_acquire)) return false;
    _items[head] = value;
    _head.store(next, std::memory_order_release);
    return true;
  }

  bool pop(T& value) {
    size_t tail = _tail.load(std::memory_order_relaxed);
    if (tail == _head.load(std::memory_order_acquire)) return false;
    value = _items[tail];
    _items[tail] = T{};
    _tail.store((tail + 1) % Capacity, std::memory_order_release);
    return true;
  }

  bool empty() const {
    return _tail.load(std::memory_order_acquire) ==
           _head.load(std::memory_order_acquire);
  }

private:
  std::array<T, Capacity> _items{};
  std::atomic<size_t> _head{0};
  std::atomic<size_t> _tail{0};
};

static UsbCdcOrderedEvent control(uint32_t epoch, uint32_t boundary,
                                  uint32_t dropped = 1) {
  UsbCdcOrderedEvent event;
  event.type = UsbCdcOrderedType::DISCONTINUITY;
  event.session = 1;
  event.epoch = epoch;
  event.byteBoundary = boundary;
  event.droppedBytes = dropped;
  return event;
}

static int stressOrderedByteBoundary() {
  static constexpr uint32_t kBytes = 200000;
  SpscRing<uint8_t, 257> bytes;
  // This is the exact channel implementation used in production. Capacity is
  // larger than the marker count here; fallback is stressed separately below.
  UsbCdcSynchronizedState<std::mutex, 257, 2> controls;
  controls.startSession(1);
  std::atomic<bool> producerDone{false};

  std::thread producer([&]() {
    uint32_t accepted = 0;
    uint32_t epoch = 0;
    for (uint32_t i = 0; i < kBytes; ++i) {
      uint8_t value = static_cast<uint8_t>(i % 251U);
      while (!bytes.push(value)) std::this_thread::yield();
      accepted++;
      if (i % 997U == 996U) {
        controls.publish(control(++epoch, accepted), true);
      }
    }
    UsbCdcOrderedEvent reset = control(++epoch, accepted, 0);
    reset.type = UsbCdcOrderedType::STREAM_RESET;
    controls.publish(reset, false);
    producerDone.store(true, std::memory_order_release);
  });

  UsbCdcOrderedCursor cursor;
  cursor.beginSession(1, 0, 0);
  uint32_t delivered = 0;
  uint32_t markers = 0;
  int failure = 0;

  while (true) {
    UsbCdcOrderedDelivery delivery;
    UsbCdcOrderedClaimResult claim;
    claim = controls.claim(cursor, delivery, false);
    if (claim == UsbCdcOrderedClaimResult::CLAIMED) {
      cursor.applyControl(delivery.event);
      markers++;
      continue;
    }

    uint8_t value = 0;
    if (bytes.pop(value)) {
      if (value != static_cast<uint8_t>(delivered % 251U)) failure = 2;
      cursor.noteByteDelivered();
      delivered++;
      continue;
    }
    // Observe producer completion before taking the final synchronized queue
    // snapshot. Reading pending first could miss the last publication and
    // then observe producerDone, terminating with one control unclaimed.
    if (producerDone.load(std::memory_order_acquire) && bytes.empty() &&
        controls.pendingCount() == 0) {
      break;
    }
    std::this_thread::yield();
  }
  producer.join();

  const uint32_t expectedLossMarkers = kBytes / 997U;
  if (delivered != kBytes || markers != expectedLossMarkers + 1 ||
      cursor.droppedBytes() != expectedLossMarkers || failure != 0) {
    std::fprintf(stderr,
                 "ordered stress failed: bytes=%u markers=%u drops=%u failure=%d\n",
                 delivered, markers, cursor.droppedBytes(), failure);
    return 1;
  }
  std::printf("USB ordered-channel stress passed: %u bytes, %u controls.\n",
              delivered, markers);
  return 0;
}

static int stressFallbackClaimAndPublish() {
  static constexpr uint32_t kControls = 100000;
  UsbCdcSynchronizedState<std::mutex, 4, 2> channel;
  channel.startSession(1);
  UsbCdcOrderedCursor cursor;
  cursor.beginSession(1, 0, 0);
  std::atomic<bool> consumerMayStart{false};
  std::atomic<uint32_t> producersDone{0};
  std::atomic<uint32_t> nextEvent{1};
  std::atomic<uint32_t> fallbackPublications{0};

  auto publishHalf = [&]() {
    for (;;) {
      uint32_t i = nextEvent.fetch_add(1, std::memory_order_relaxed);
      if (i > kControls) break;
      UsbCdcOrderedEvent event = control(i, 0);
      if (i % 997U == 0) event.type = UsbCdcOrderedType::STREAM_RESET;
      UsbCdcOrderedPublishResult result = channel.publish(event, false);
      if (result != UsbCdcOrderedPublishResult::QUEUED) {
        fallbackPublications.fetch_add(1, std::memory_order_relaxed);
      }
      if (i == 1000) consumerMayStart.store(true, std::memory_order_release);
    }
    producersDone.fetch_add(1, std::memory_order_release);
  };
  std::thread producerA(publishHalf);
  std::thread producerB(publishHalf);

  while (!consumerMayStart.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  uint32_t claimed = 0;
  while (true) {
    UsbCdcOrderedDelivery delivery;
    UsbCdcOrderedClaimResult result;
    result = channel.claim(cursor, delivery, false);
    if (result == UsbCdcOrderedClaimResult::CLAIMED) {
      cursor.applyControl(delivery.event);
      claimed++;
      continue;
    }
    if (producersDone.load(std::memory_order_acquire) == 2 &&
        channel.pendingCount() == 0) break;
    std::this_thread::yield();
  }
  producerA.join();
  producerB.join();

  if (fallbackPublications.load() == 0 || claimed == 0 ||
      cursor.droppedBytes() != kControls) {
    std::fprintf(stderr,
                 "fallback stress failed: fallback=%u claimed=%u drops=%u\n",
                 fallbackPublications.load(), claimed, cursor.droppedBytes());
    return 2;
  }
  std::printf("USB fallback stress passed: %u controls, %u atomic claims.\n",
              kControls, claimed);
  return 0;
}

static int stressTerminalVsConfigGate() {
  static constexpr uint32_t kSessions = 50000;
  UsbCdcSynchronizedState<std::mutex, 4, 2> shared;
  std::atomic<uint32_t> armed{0};
  std::atomic<uint32_t> terminalDone{0};
  std::atomic<size_t> activeSlot{0};

  std::thread callback([&]() {
    for (uint32_t session = 1; session <= kSessions; ++session) {
      while (armed.load(std::memory_order_acquire) != session) {
        std::this_thread::yield();
      }
      {
        auto lease = shared.acquireCallback(
          activeSlot.load(std::memory_order_acquire), session);
        if (lease) shared.latchTerminal(session);
      }
      terminalDone.store(session, std::memory_order_release);
    }
  });

  for (uint32_t session = 1; session <= kSessions; ++session) {
    shared.startSession(session);
    int slot = shared.acquireContext(session);
    if (slot < 0) return 3;
    UsbCdcConfigToken token = shared.configurationToken();
    activeSlot.store(static_cast<size_t>(slot), std::memory_order_release);
    armed.store(session, std::memory_order_release);
    if ((session & 1U) == 0) std::this_thread::yield();
    shared.commitConfiguration(token, UsbCdcOrderedPublishResult::QUEUED);
    while (terminalDone.load(std::memory_order_acquire) != session) {
      std::this_thread::yield();
    }
    if (shared.callbackEnabled(static_cast<size_t>(slot), session)) return 4;
    if (!shared.retireContext(static_cast<size_t>(slot), session)) return 5;
    if (shared.finishClose(static_cast<size_t>(slot), session, true) !=
        UsbCdcCloseCompletion::RELEASED) return 6;
  }
  callback.join();
  std::printf("USB session-gate stress passed: %u terminal/config races.\n",
              kSessions);
  return 0;
}

static int deterministicAdmittedByteVsTerminalBoundary() {
  UsbCdcSynchronizedState<std::mutex, 4, 1> shared;
  shared.startSession(1);
  int slot = shared.acquireContext(1);
  if (slot < 0) return 7;
  UsbCdcConfigToken token = shared.configurationToken();
  if (!shared.commitConfiguration(token,
                                  UsbCdcOrderedPublishResult::QUEUED)) {
    return 8;
  }

  std::atomic<bool> byteAdmitted{false};
  std::atomic<bool> releaseByte{false};
  std::atomic<bool> terminalLatched{false};
  std::atomic<bool> boundaryReady{false};
  std::atomic<uint32_t> acceptedBytes{0};
  std::atomic<uint32_t> terminalBoundary{0};

  std::thread callback([&]() {
    auto callbackLease = shared.acquireCallback(static_cast<size_t>(slot), 1);
    UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
    auto byteLease = shared.acquireByteCommitOrRecordDrop(
      static_cast<size_t>(slot), 1, 0, 0, admission);
    if (!callbackLease || !byteLease) return;
    byteAdmitted.store(true, std::memory_order_release);
    while (!releaseByte.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    acceptedBytes.fetch_add(1, std::memory_order_release);
  });

  std::thread terminal([&]() {
    while (!byteAdmitted.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    shared.latchTerminal(1);
    terminalLatched.store(true, std::memory_order_release);
    while (!shared.callbacksQuiescent(static_cast<size_t>(slot), 1)) {
      std::this_thread::yield();
    }
    terminalBoundary.store(acceptedBytes.load(std::memory_order_acquire),
                           std::memory_order_release);
    boundaryReady.store(true, std::memory_order_release);
  });

  while (!terminalLatched.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  for (size_t attempt = 0; attempt < 1000; ++attempt) {
    if (boundaryReady.load(std::memory_order_acquire)) return 9;
    std::this_thread::yield();
  }
  releaseByte.store(true, std::memory_order_release);
  callback.join();
  terminal.join();

  if (!boundaryReady.load(std::memory_order_acquire) ||
      terminalBoundary.load(std::memory_order_acquire) != 1 ||
      acceptedBytes.load(std::memory_order_acquire) != 1) {
    return 10;
  }
  auto lateCallback = shared.acquireCallback(static_cast<size_t>(slot), 1);
  if (!lateCallback) return 11;
  UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
  if (shared.acquireByteCommitOrRecordDrop(
        static_cast<size_t>(slot), 1, 0, 1, admission)) return 12;
  if (admission != UsbCdcByteAdmission::REJECTED_TERMINAL) return 13;
  std::printf("USB admitted-byte terminal barrier passed.\n");
  return 0;
}

static int deterministicQuarantineClaimRace() {
  static constexpr uint32_t kIterations = 20000;
  for (uint32_t iteration = 1; iteration <= kIterations; ++iteration) {
    UsbCdcSynchronizedState<std::mutex, 2, 1> shared;
    shared.startSession(1);
    int slot = shared.acquireContext(1);
    if (slot < 0) return 14;
    UsbCdcConfigToken token = shared.configurationToken();
    if (!shared.commitConfiguration(token,
                                    UsbCdcOrderedPublishResult::QUEUED)) {
      return 15;
    }
    UsbCdcOrderedEvent marker = control(1, 0, 2);
    if (shared.publishQuarantineBarrier(marker) !=
        UsbCdcOrderedPublishResult::QUEUED) {
      return 16;
    }

    UsbCdcOrderedCursor cursor;
    cursor.beginSession(1, 0, 0);
    UsbCdcOrderedDelivery delivery;
    std::atomic<bool> start{false};
    std::atomic<int> admissionValue{
      static_cast<int>(UsbCdcByteAdmission::REJECTED_INACTIVE)};
    std::atomic<bool> leaseGranted{false};

    std::thread callback([&]() {
      auto callbackLease = shared.acquireCallback(static_cast<size_t>(slot), 1);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      UsbCdcByteAdmission byteAdmission =
        UsbCdcByteAdmission::REJECTED_INACTIVE;
      auto byteLease = shared.acquireByteCommitOrRecordDrop(
        static_cast<size_t>(slot), 1, 1, 5, byteAdmission);
      leaseGranted.store(static_cast<bool>(byteLease),
                         std::memory_order_release);
      admissionValue.store(static_cast<int>(byteAdmission),
                           std::memory_order_release);
    });
    std::thread consumer([&]() {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      while (shared.claim(cursor, delivery, true) ==
             UsbCdcOrderedClaimResult::BLOCKED) {
        std::this_thread::yield();
      }
    });
    start.store(true, std::memory_order_release);
    callback.join();
    consumer.join();

    UsbCdcByteAdmission result = static_cast<UsbCdcByteAdmission>(
      admissionValue.load(std::memory_order_acquire));
    bool admitted = leaseGranted.load(std::memory_order_acquire);
    if (admitted) {
      if (result != UsbCdcByteAdmission::ADMITTED ||
          delivery.event.droppedBytes != 2) return 17;
    } else if (result == UsbCdcByteAdmission::DROP_RECORDED) {
      if (delivery.event.droppedBytes != 7) return 18;
    } else {
      return 19;
    }
  }
  std::printf("USB quarantine/claim race passed: %u barriers.\n", kIterations);
  return 0;
}

static int deterministicQueuedResumeWithFallbackRace() {
  static constexpr uint32_t kIterations = 5000;
  for (uint32_t iteration = 1; iteration <= kIterations; ++iteration) {
    UsbCdcSynchronizedState<std::mutex, 1, 1> shared;
    shared.startSession(1);
    int slot = shared.acquireContext(1);
    if (slot < 0) return 20;
    UsbCdcConfigToken token = shared.configurationToken();
    if (!shared.commitConfiguration(token,
                                    UsbCdcOrderedPublishResult::QUEUED)) {
      return 21;
    }
    UsbCdcOrderedEvent first = control(1, 0, 2);
    UsbCdcOrderedEvent fallback = control(1, 0, 3);
    if (shared.publishQuarantineBarrier(first) !=
          UsbCdcOrderedPublishResult::QUEUED ||
        shared.publishQuarantineBarrier(fallback) !=
          UsbCdcOrderedPublishResult::FALLBACK_CREATED) {
      return 22;
    }

    UsbCdcOrderedCursor cursor;
    cursor.beginSession(1, 0, 0);
    UsbCdcOrderedDelivery firstDelivery;
    std::atomic<uint32_t> resumeHooks{0};
    std::atomic<bool> start{false};
    std::atomic<bool> admitted{false};
    std::atomic<int> disposition{
      static_cast<int>(UsbCdcByteAdmission::REJECTED_INACTIVE)};

    std::thread callback([&]() {
      auto callbackLease = shared.acquireCallback(static_cast<size_t>(slot), 1);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      UsbCdcByteAdmission result = UsbCdcByteAdmission::REJECTED_INACTIVE;
      auto byteLease = shared.acquireByteCommitOrRecordDrop(
        static_cast<size_t>(slot), 1, 1, 5, result);
      admitted.store(static_cast<bool>(byteLease), std::memory_order_release);
      disposition.store(static_cast<int>(result), std::memory_order_release);
    });
    std::thread consumer([&]() {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      shared.claim(cursor, firstDelivery, true, [&]() {
        resumeHooks.fetch_add(1, std::memory_order_relaxed);
      });
    });
    start.store(true, std::memory_order_release);
    callback.join();
    consumer.join();

    if (admitted.load(std::memory_order_acquire) ||
        static_cast<UsbCdcByteAdmission>(
          disposition.load(std::memory_order_acquire)) !=
          UsbCdcByteAdmission::DROP_RECORDED ||
        !shared.quarantined() || resumeHooks.load(std::memory_order_acquire) != 0) {
      return 23;
    }
    cursor.applyControl(firstDelivery.event);
    UsbCdcOrderedDelivery fallbackDelivery;
    if (shared.claim(cursor, fallbackDelivery, true, [&]() {
          resumeHooks.fetch_add(1, std::memory_order_relaxed);
        }) !=
        UsbCdcOrderedClaimResult::CLAIMED) {
      return 24;
    }
    if (firstDelivery.event.droppedBytes +
          fallbackDelivery.event.droppedBytes != 10 ||
        shared.quarantined() || resumeHooks.load(std::memory_order_acquire) != 1) {
      return 25;
    }
    auto callbackLease = shared.acquireCallback(static_cast<size_t>(slot), 1);
    UsbCdcByteAdmission result = UsbCdcByteAdmission::REJECTED_INACTIVE;
    if (!shared.acquireByteCommitOrRecordDrop(
          static_cast<size_t>(slot), 1, 1, 0, result) ||
        result != UsbCdcByteAdmission::ADMITTED) {
      return 26;
    }
  }
  std::printf("USB queued-resume/fallback race passed: %u barriers.\n",
              kIterations);
  return 0;
}

int main() {
  int result = stressOrderedByteBoundary();
  if (result != 0) return result;
  result = stressFallbackClaimAndPublish();
  if (result != 0) return result;
  result = stressTerminalVsConfigGate();
  if (result != 0) return result;
  result = deterministicAdmittedByteVsTerminalBoundary();
  if (result != 0) return result;
  result = deterministicQuarantineClaimRace();
  if (result != 0) return result;
  return deterministicQueuedResumeWithFallbackRace();
}
