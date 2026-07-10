#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "lib/transports/UsbCdcState.h"
#include "test_support.h"

static UsbCdcControlEvent event(UsbCdcControlType type, int32_t code = 0,
                                uint32_t count = 0) {
  UsbCdcControlEvent value;
  value.type = type;
  value.code = code;
  value.count = count;
  return value;
}

static void installReady(UsbCdcLifecycle& state) {
  state.apply(event(UsbCdcControlType::BEGIN), 0);
  state.apply(event(UsbCdcControlType::HOST_INSTALL_OK), 0);
  state.apply(event(UsbCdcControlType::DRIVER_INSTALL_OK), 0);
}

static void connectReady(UsbCdcLifecycle& state, uint64_t now = 0) {
  installReady(state);
  state.apply(event(UsbCdcControlType::DEVICE_ATTACHED), now);
  state.apply(event(UsbCdcControlType::OPEN_STARTED), now);
  state.apply(event(UsbCdcControlType::OPEN_SUCCEEDED), now);
  state.apply(event(UsbCdcControlType::CONFIG_SUCCEEDED), now);
}

static void testInstallFailureRetriesWithBackoff() {
  UsbCdcLifecycle state;
  state.apply(event(UsbCdcControlType::BEGIN), 0);
  CHECK_EQ(static_cast<int>(state.phase()),
           static_cast<int>(UsbCdcPhase::INSTALLING), "begin -> installing");

  state.apply(event(UsbCdcControlType::HOST_INSTALL_FAILED, -11), 100);
  CHECK_EQ(static_cast<int>(state.phase()),
           static_cast<int>(UsbCdcPhase::RETRY_WAIT), "install fail -> retry wait");
  CHECK_EQ(static_cast<int>(state.retryTarget()),
           static_cast<int>(UsbCdcRetryTarget::INSTALL), "retry targets install");
  CHECK_EQ(state.lastError(), -11, "install error retained");
  CHECK_EQ(state.retryAtMs(), static_cast<uint64_t>(1100),
           "first retry uses one-second backoff");

  state.apply(event(UsbCdcControlType::RETRY_TICK), 1099);
  CHECK_EQ(static_cast<int>(state.phase()),
           static_cast<int>(UsbCdcPhase::RETRY_WAIT), "early tick does nothing");
  state.apply(event(UsbCdcControlType::RETRY_TICK), 1100);
  CHECK_EQ(static_cast<int>(state.phase()),
           static_cast<int>(UsbCdcPhase::INSTALLING), "due tick retries install");

  state.apply(event(UsbCdcControlType::HOST_INSTALL_OK), 1100);
  CHECK_TRUE(state.hostReady(), "host install success retained");
  state.apply(event(UsbCdcControlType::DRIVER_INSTALL_FAILED, -22), 1200);
  CHECK_TRUE(!state.hostReady() && !state.driverReady(),
             "driver failure reflects daemon cleanup");
  CHECK_EQ(state.retryAtMs(), static_cast<uint64_t>(3200),
           "second install failure uses bounded exponential backoff");
}

static void testOpenConfigureAndRetry() {
  UsbCdcLifecycle state;
  installReady(state);
  CHECK_EQ(static_cast<int>(state.phase()),
           static_cast<int>(UsbCdcPhase::WAITING_DEVICE), "driver ready waits device");
  state.apply(event(UsbCdcControlType::DEVICE_ATTACHED), 10);
  CHECK_TRUE(state.deviceAttached(), "attach retained");
  CHECK_TRUE(state.shouldAttemptOpen(10), "attached ready device may open");

  state.apply(event(UsbCdcControlType::OPEN_STARTED), 10);
  CHECK_TRUE(state.openInProgress(), "open is explicit");
  state.apply(event(UsbCdcControlType::OPEN_FAILED, -33), 20);
  CHECK_TRUE(!state.openInProgress() && !state.handleOwned(),
             "failed open owns no handle");
  CHECK_EQ(static_cast<int>(state.retryTarget()),
           static_cast<int>(UsbCdcRetryTarget::OPEN), "open retry classified");

  state.apply(event(UsbCdcControlType::RETRY_TICK), state.retryAtMs());
  state.apply(event(UsbCdcControlType::OPEN_STARTED), state.retryAtMs());
  state.apply(event(UsbCdcControlType::OPEN_SUCCEEDED), state.retryAtMs());
  CHECK_TRUE(state.handleOwned() && !state.connected(),
             "open owns handle but waits for configuration");
  CHECK_EQ(static_cast<int>(state.phase()),
           static_cast<int>(UsbCdcPhase::CONFIGURING), "open -> configuring");

  state.apply(event(UsbCdcControlType::CONFIG_FAILED, -44), state.retryAtMs());
  CHECK_TRUE(state.closePending(), "config failure requests owner-side close");
  CHECK_TRUE(state.takeCloseRequest(), "close request consumed once");
  CHECK_TRUE(!state.takeCloseRequest(), "duplicate close request suppressed");
  state.apply(event(UsbCdcControlType::HANDLE_CLOSED), state.retryAtMs());
  CHECK_TRUE(!state.handleOwned(), "owner close clears handle ownership");
}

static void testErrorDisconnectClosesOnceAndReconnects() {
  UsbCdcLifecycle state;
  connectReady(state);
  CHECK_TRUE(state.connected(), "fixture connected");
  CHECK_EQ(state.reconnectCount(), 0U, "first connection is not reconnect");

  state.apply(event(UsbCdcControlType::TRANSFER_ERROR, -55), 100);
  CHECK_TRUE(!state.connected(), "transfer error leaves disconnected state");
  CHECK_EQ(state.dataLossEpisodes(), 1U, "error begins one loss episode");
  CHECK_EQ(state.rxEpoch(), 1U, "error advances RX epoch");
  state.apply(event(UsbCdcControlType::DEVICE_DISCONNECTED), 101);
  CHECK_EQ(state.dataLossEpisodes(), 1U,
           "error followed by disconnect remains one loss episode");
  CHECK_TRUE(!state.deviceAttached(), "disconnect clears attachment");
  CHECK_TRUE(state.takeCloseRequest(), "error/disconnect requests one close");
  CHECK_TRUE(!state.takeCloseRequest(), "error/disconnect cannot close twice");
  state.apply(event(UsbCdcControlType::HANDLE_CLOSED), 101);

  state.apply(event(UsbCdcControlType::DEVICE_ATTACHED), 200);
  state.apply(event(UsbCdcControlType::OPEN_STARTED), 200);
  state.apply(event(UsbCdcControlType::OPEN_SUCCEEDED), 200);
  state.apply(event(UsbCdcControlType::CONFIG_SUCCEEDED), 200);
  CHECK_TRUE(state.connected(), "device reconnects after owner close");
  CHECK_EQ(state.reconnectCount(), 1U, "successful reconnection counted once");
}

static void testOverflowCountersAndRecovery() {
  UsbCdcLifecycle state;
  connectReady(state);
  state.apply(event(UsbCdcControlType::RX_OVERFLOW, 0, 3), 10);
  CHECK_EQ(state.dataLossEpisodes(), 1U, "first overflow starts episode");
  CHECK_EQ(state.overflowEpisodes(), 1U, "overflow episode counted");
  CHECK_EQ(state.droppedBytes(), 3U, "first dropped bytes exact");
  CHECK_EQ(state.rxEpoch(), 1U, "first overflow advances epoch");

  state.apply(event(UsbCdcControlType::RX_OVERFLOW, 0, 5), 11);
  CHECK_EQ(state.overflowEpisodes(), 1U, "same saturation coalesces episode");
  CHECK_EQ(state.droppedBytes(), 8U, "same episode accumulates exact bytes");
  CHECK_EQ(state.rxEpoch(), 1U, "same overflow episode keeps epoch");

  state.apply(event(UsbCdcControlType::RX_CAPACITY_RECOVERED), 12);
  state.apply(event(UsbCdcControlType::RX_OVERFLOW, 0, 2), 13);
  CHECK_EQ(state.overflowEpisodes(), 2U, "second saturation is independent");
  CHECK_EQ(state.dataLossEpisodes(), 2U, "second loss episode counted");
  CHECK_EQ(state.droppedBytes(), 10U, "lifetime dropped bytes exact");
  CHECK_EQ(state.rxEpoch(), 2U, "second episode advances epoch");
}

static void testPodQueuesReserveCriticalCapacity() {
  CHECK_TRUE(std::is_trivially_copyable<UsbCdcControlEvent>::value,
             "callback control event is POD");
  CHECK_TRUE(std::is_standard_layout<UsbCdcControlEvent>::value,
             "callback control event is standard layout");
  CHECK_TRUE(std::is_trivially_copyable<UsbCdcOrderedEvent>::value,
             "ordered stream control is POD");
  CHECK_TRUE(std::is_standard_layout<UsbCdcOrderedEvent>::value,
             "ordered stream control is standard layout");
  CHECK_TRUE(!usbCdcMayEnqueueNormalControl(
               USB_CDC_LIFECYCLE_CRITICAL_RESERVE),
             "normal notice cannot consume critical reserve");
  CHECK_TRUE(usbCdcMayEnqueueCriticalControl(
               USB_CDC_LIFECYCLE_CRITICAL_RESERVE),
             "critical control fits after normal admission stops");
  CHECK_TRUE(!usbCdcMayEnqueueCriticalControl(0),
             "zero space requires epoch fallback rather than silent loss");
}

static void testClosePendingDoesNotConsumeRetry() {
  UsbCdcLifecycle state;
  installReady(state);
  state.apply(event(UsbCdcControlType::DEVICE_ATTACHED), 0);
  state.apply(event(UsbCdcControlType::OPEN_STARTED), 0);
  state.apply(event(UsbCdcControlType::OPEN_SUCCEEDED), 0);
  state.apply(event(UsbCdcControlType::CONFIG_FAILED, -60), 100);
  uint64_t due = state.retryAtMs();
  state.apply(event(UsbCdcControlType::RETRY_TICK), due);
  CHECK_EQ(static_cast<int>(state.retryTarget()),
           static_cast<int>(UsbCdcRetryTarget::OPEN),
           "due retry remains pending until owner closes handle");
  CHECK_EQ(static_cast<int>(state.phase()),
           static_cast<int>(UsbCdcPhase::RETRY_WAIT),
           "close-pending retry does not transition early");
  CHECK_TRUE(state.takeCloseRequest(), "owner consumes config-failure close");
  state.apply(event(UsbCdcControlType::HANDLE_CLOSED), due + 1);
  CHECK_TRUE(state.shouldAttemptOpen(due + 1),
             "overdue open retry becomes runnable after close");
}

static void testStaleOpenAndConfigCannotResurrectSession() {
  UsbCdcLifecycle state;
  installReady(state);
  state.apply(event(UsbCdcControlType::DEVICE_ATTACHED), 0);
  state.apply(event(UsbCdcControlType::OPEN_STARTED), 0);
  state.apply(event(UsbCdcControlType::DEVICE_DISCONNECTED), 1);
  state.apply(event(UsbCdcControlType::OPEN_SUCCEEDED), 2);
  CHECK_TRUE(state.handleOwned(), "stale successful open is temporarily owned");
  CHECK_TRUE(state.closePending(), "stale successful open is closed by owner");
  CHECK_TRUE(!state.connected(), "stale open never connects");
  CHECK_TRUE(static_cast<int>(state.phase()) !=
               static_cast<int>(UsbCdcPhase::CONFIGURING),
             "stale open never starts configuration");
  CHECK_TRUE(state.takeCloseRequest(), "stale handle closes exactly once");
  CHECK_TRUE(!state.takeCloseRequest(), "stale handle has no duplicate close");

  UsbCdcLifecycle config;
  installReady(config);
  config.apply(event(UsbCdcControlType::DEVICE_ATTACHED), 0);
  config.apply(event(UsbCdcControlType::OPEN_STARTED), 0);
  config.apply(event(UsbCdcControlType::OPEN_SUCCEEDED), 0);
  config.apply(event(UsbCdcControlType::DEVICE_DISCONNECTED), 1);
  config.apply(event(UsbCdcControlType::CONFIG_SUCCEEDED), 2);
  CHECK_TRUE(!config.connected(), "stale config success cannot resurrect READY");
  CHECK_TRUE(static_cast<int>(config.phase()) !=
               static_cast<int>(UsbCdcPhase::READY),
             "stale config result retains terminal state");
}

static void testTerminalEventsAreIdempotentAndDistinctFromOverflow() {
  UsbCdcLifecycle duplicate;
  connectReady(duplicate);
  duplicate.apply(event(UsbCdcControlType::TRANSFER_ERROR, -70), 100);
  uint64_t retryAt = duplicate.retryAtMs();
  duplicate.apply(event(UsbCdcControlType::TRANSFER_ERROR, -71), 101);
  CHECK_EQ(duplicate.rxEpoch(), 1U, "duplicate error keeps one terminal epoch");
  CHECK_EQ(duplicate.dataLossEpisodes(), 1U,
           "duplicate error keeps one terminal loss episode");
  CHECK_EQ(duplicate.retryAtMs(), retryAt,
           "duplicate error does not extend retry backoff");

  UsbCdcLifecycle overflowThenError;
  connectReady(overflowThenError);
  overflowThenError.apply(event(UsbCdcControlType::RX_OVERFLOW, 0, 2), 10);
  overflowThenError.apply(event(UsbCdcControlType::TRANSFER_ERROR, -72), 11);
  CHECK_EQ(overflowThenError.rxEpoch(), 2U,
           "terminal error advances epoch after overflow episode");
  CHECK_EQ(overflowThenError.dataLossEpisodes(), 2U,
           "overflow and terminal break are distinct losses");

  UsbCdcLifecycle errorThenStaleCapacity;
  connectReady(errorThenStaleCapacity);
  errorThenStaleCapacity.apply(event(UsbCdcControlType::TRANSFER_ERROR, -73), 20);
  errorThenStaleCapacity.apply(event(UsbCdcControlType::RX_CAPACITY_RECOVERED), 21);
  errorThenStaleCapacity.apply(event(UsbCdcControlType::DEVICE_DISCONNECTED), 22);
  CHECK_EQ(errorThenStaleCapacity.rxEpoch(), 1U,
           "stale capacity event cannot split terminal incident");
  CHECK_EQ(errorThenStaleCapacity.dataLossEpisodes(), 1U,
           "error/disconnect remains one incident after stale capacity notice");
}

static void testConfigAndControlFailuresRecover() {
  UsbCdcLifecycle config;
  installReady(config);
  config.apply(event(UsbCdcControlType::DEVICE_ATTACHED), 0);
  config.apply(event(UsbCdcControlType::OPEN_STARTED), 0);
  config.apply(event(UsbCdcControlType::OPEN_SUCCEEDED), 0);
  config.apply(event(UsbCdcControlType::CONFIG_FAILED, -80), 1);
  CHECK_EQ(config.rxEpoch(), 1U, "config failure resets stream epoch");
  CHECK_EQ(config.dataLossEpisodes(), 1U, "config failure is terminal loss");

  UsbCdcLifecycle control;
  connectReady(control);
  control.apply(event(UsbCdcControlType::CONTROL_QUEUE_OVERFLOW, -81), 100);
  CHECK_TRUE(control.closePending(), "control queue failure closes fail-closed");
  CHECK_EQ(static_cast<int>(control.phase()),
           static_cast<int>(UsbCdcPhase::RETRY_WAIT),
           "control queue failure enters bounded recovery");
  CHECK_EQ(static_cast<int>(control.retryTarget()),
           static_cast<int>(UsbCdcRetryTarget::OPEN),
           "connected control failure retries device open");
}

static void testInstallRecoveryResetsBackoffAndCapsIt() {
  UsbCdcLifecycle state;
  state.apply(event(UsbCdcControlType::BEGIN), 0);
  state.apply(event(UsbCdcControlType::HOST_INSTALL_FAILED, -90), 0);
  state.apply(event(UsbCdcControlType::RETRY_TICK), 1000);
  state.apply(event(UsbCdcControlType::HOST_INSTALL_OK), 1000);
  state.apply(event(UsbCdcControlType::DRIVER_INSTALL_OK), 1000);
  CHECK_EQ(static_cast<int>(state.phase()),
           static_cast<int>(UsbCdcPhase::WAITING_DEVICE),
           "install-fails-once fully recovers");

  state.apply(event(UsbCdcControlType::HOST_INSTALL_FAILED, -91), 2000);
  CHECK_EQ(state.retryAtMs(), static_cast<uint64_t>(3000),
           "successful install resets backoff attempt");

  uint64_t now = state.retryAtMs();
  for (int i = 0; i < 8; ++i) {
    state.apply(event(UsbCdcControlType::RETRY_TICK), now);
    state.apply(event(UsbCdcControlType::HOST_INSTALL_FAILED, -92), now);
    CHECK_TRUE(state.retryAtMs() - now <= 16000,
               "install retry backoff is bounded at 16 seconds");
    now = state.retryAtMs();
  }
}

static void testOrderedByteBoundaryCursor() {
  CHECK_TRUE(std::is_trivially_copyable<UsbCdcOrderedEvent>::value,
             "ordered RX control is POD");
  CHECK_TRUE(std::is_standard_layout<UsbCdcOrderedEvent>::value,
             "ordered RX control is standard layout");

  UsbCdcOrderedCursor cursor;
  cursor.beginSession(7, 0, 0);
  UsbCdcOrderedEvent loss;
  loss.type = UsbCdcOrderedType::DISCONTINUITY;
  loss.session = 7;
  loss.epoch = 1;
  loss.byteBoundary = 3;
  loss.droppedBytes = 5;
  CHECK_TRUE(!cursor.controlDue(loss),
             "loss marker waits behind previously accepted bytes");
  cursor.noteByteDelivered();
  cursor.noteByteDelivered();
  CHECK_TRUE(!cursor.controlDue(loss), "marker still waits at boundary minus one");
  cursor.noteByteDelivered();
  CHECK_TRUE(cursor.controlDue(loss), "marker becomes due at exact byte boundary");
  cursor.applyControl(loss);
  CHECK_EQ(cursor.epoch(), 1U, "applied marker advances delivered epoch");
  CHECK_EQ(cursor.droppedBytes(), 5U, "ordered cursor accumulates exact drops");

  UsbCdcOrderedEvent stale = loss;
  stale.session = 6;
  stale.byteBoundary = cursor.deliveredByteSequence();
  CHECK_TRUE(cursor.controlStale(stale), "prior-session marker is stale");
  CHECK_TRUE(!cursor.controlDue(stale), "stale marker can never block new session");

  UsbCdcOrderedEvent nextSession = loss;
  nextSession.session = 8;
  nextSession.byteBoundary = cursor.deliveredByteSequence();
  CHECK_TRUE(cursor.controlDue(nextSession),
             "next-session reset is allowed at an exact boundary");
  cursor.applyControl(nextSession);
  CHECK_EQ(cursor.session(), 8U, "next-session reset advances cursor session");

  cursor.beginSession(8, UINT32_MAX, 9);
  cursor.noteByteDelivered();
  UsbCdcOrderedEvent wrapped;
  wrapped.type = UsbCdcOrderedType::STREAM_RESET;
  wrapped.session = 8;
  wrapped.epoch = 10;
  wrapped.byteBoundary = 0;
  CHECK_TRUE(cursor.controlDue(wrapped),
             "uint32 byte boundary remains exact across wrap");
  cursor.applyControl(wrapped);
  CHECK_EQ(cursor.epoch(), 10U, "wrapped reset publishes new epoch");
}

int main() {
  testInstallFailureRetriesWithBackoff();
  testOpenConfigureAndRetry();
  testErrorDisconnectClosesOnceAndReconnects();
  testOverflowCountersAndRecovery();
  testPodQueuesReserveCriticalCapacity();
  testClosePendingDoesNotConsumeRetry();
  testStaleOpenAndConfigCannotResurrectSession();
  testTerminalEventsAreIdempotentAndDistinctFromOverflow();
  testConfigAndControlFailuresRecover();
  testInstallRecoveryResetsBackoffAndCapsIt();
  testOrderedByteBoundaryCursor();
  return testReport();
}
