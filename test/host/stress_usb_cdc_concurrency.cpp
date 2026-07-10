#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

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

int main() {
  static constexpr uint32_t kBytes = 200000;
  SpscRing<uint8_t, 257> bytes;
  SpscRing<UsbCdcOrderedEvent, 17> controls;
  std::atomic<bool> producerDone{false};
  std::atomic<int> producerFailure{0};

  std::thread producer([&]() {
    uint32_t accepted = 0;
    uint32_t epoch = 0;
    for (uint32_t i = 0; i < kBytes; ++i) {
      uint8_t value = static_cast<uint8_t>(i % 251U);
      while (!bytes.push(value)) std::this_thread::yield();
      accepted++;
      if (i % 997U == 996U) {
        UsbCdcOrderedEvent event;
        event.type = UsbCdcOrderedType::DISCONTINUITY;
        event.session = 1;
        event.epoch = ++epoch;
        event.byteBoundary = accepted;
        event.droppedBytes = 1;
        while (!controls.push(event)) std::this_thread::yield();
      }
    }
    UsbCdcOrderedEvent reset;
    reset.type = UsbCdcOrderedType::STREAM_RESET;
    reset.session = 1;
    reset.epoch = ++epoch;
    reset.byteBoundary = accepted;
    if (reset.byteBoundary != kBytes) producerFailure.store(1);
    while (!controls.push(reset)) std::this_thread::yield();
    producerDone.store(true, std::memory_order_release);
  });

  UsbCdcOrderedCursor cursor;
  cursor.beginSession(1, 0, 0);
  UsbCdcOrderedEvent heldControl;
  bool controlHeld = false;
  uint32_t delivered = 0;
  uint32_t markers = 0;
  int failure = 0;

  while (!producerDone.load(std::memory_order_acquire) || !bytes.empty() ||
         !controls.empty() || controlHeld) {
    if (!controlHeld) controlHeld = controls.pop(heldControl);
    if (controlHeld && cursor.controlDue(heldControl)) {
      cursor.applyControl(heldControl);
      heldControl = UsbCdcOrderedEvent{};
      controlHeld = false;
      markers++;
      continue;
    }

    uint8_t value = 0;
    if (bytes.pop(value)) {
      if (value != static_cast<uint8_t>(delivered % 251U)) failure = 2;
      cursor.noteByteDelivered();
      delivered++;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();

  const uint32_t expectedLossMarkers = kBytes / 997U;
  if (producerFailure.load() != 0 || delivered != kBytes ||
      markers != expectedLossMarkers + 1 ||
      cursor.droppedBytes() != expectedLossMarkers || failure != 0) {
    std::fprintf(stderr,
                 "stress failed: bytes=%u markers=%u drops=%u failure=%d\n",
                 delivered, markers, cursor.droppedBytes(), failure);
    return 1;
  }
  std::printf("USB concurrency stress passed: %u bytes, %u ordered controls.\n",
              delivered, markers);
  return 0;
}
