#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>

#include "lib/transports/UsbCdcConcurrency.h"
#include "lib/transports/UsbCdcState.h"
#include "src/third_party/espressif_usb_host_cdc_acm/cdc_notification_parser.h"
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

static UsbCdcOrderedEvent orderedEvent(UsbCdcOrderedType type,
                                       uint32_t session, uint32_t epoch,
                                       uint32_t boundary,
                                       uint32_t dropped = 0) {
  UsbCdcOrderedEvent value;
  value.type = type;
  value.session = session;
  value.epoch = epoch;
  value.byteBoundary = boundary;
  value.droppedBytes = dropped;
  return value;
}

static void testOrderedChannelFallbackIsAtomicAndOrdered() {
  UsbCdcOrderedChannel<2> channel;
  CHECK_EQ(static_cast<int>(channel.publish(
             orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 1, 1, 0, 1),
             true)),
           static_cast<int>(UsbCdcOrderedPublishResult::QUEUED),
           "first ordered control is queued");
  CHECK_EQ(static_cast<int>(channel.publish(
             orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 1, 2, 0, 2),
             true)),
           static_cast<int>(UsbCdcOrderedPublishResult::QUEUED),
           "second ordered control fills channel");
  CHECK_EQ(static_cast<int>(channel.publish(
             orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 1, 3, 0, 3),
             true)),
           static_cast<int>(UsbCdcOrderedPublishResult::FALLBACK_CREATED),
           "full channel creates fail-closed fallback");
  CHECK_EQ(static_cast<int>(channel.publish(
             orderedEvent(UsbCdcOrderedType::STREAM_RESET, 2, 4, 0, 4),
             false)),
           static_cast<int>(UsbCdcOrderedPublishResult::FALLBACK_MERGED),
           "newer reset merges behind older queued controls");
  CHECK_EQ(channel.pendingCount(), static_cast<size_t>(3),
           "fallback remains ordered after queued controls");

  UsbCdcOrderedCursor cursor;
  cursor.beginSession(1, 0, 0);
  UsbCdcOrderedDelivery delivery;
  CHECK_EQ(static_cast<int>(channel.claim(cursor, delivery)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "oldest queued control claims first");
  CHECK_TRUE(!delivery.fromFallback && delivery.event.epoch == 1,
             "first queue item retains identity");
  cursor.applyControl(delivery.event);
  CHECK_EQ(static_cast<int>(channel.claim(cursor, delivery)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "second queued control claims second");
  cursor.applyControl(delivery.event);
  CHECK_EQ(static_cast<int>(channel.claim(cursor, delivery)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "fallback claims only after queue drains");
  CHECK_TRUE(delivery.fromFallback, "fallback claim is explicit");
  CHECK_EQ(static_cast<int>(delivery.event.type),
           static_cast<int>(UsbCdcOrderedType::STREAM_RESET),
           "terminal reset dominates merged discontinuity");
  CHECK_EQ(delivery.event.session, 2U, "fallback advances to newer session");
  CHECK_EQ(delivery.event.droppedBytes, 7U,
           "fallback merge preserves exact dropped-byte total");
  CHECK_TRUE(!delivery.resumeProducer,
             "terminal reset suppresses an earlier resume request");
  cursor.applyControl(delivery.event);
  CHECK_EQ(channel.pendingCount(), static_cast<size_t>(0),
           "atomic claim clears exactly the claimed fallback");

  CHECK_EQ(static_cast<int>(channel.publish(
             orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 1, 5, 0), false)),
           static_cast<int>(UsbCdcOrderedPublishResult::QUEUED),
           "stale probe queues");
  CHECK_EQ(static_cast<int>(channel.claim(cursor, delivery)),
           static_cast<int>(UsbCdcOrderedClaimResult::STALE_QUEUE_DISCARDED),
           "stale queued controls cannot block the active session");

  channel.clear();
  cursor.beginSession(3, 0, cursor.epoch());
  channel.publish(
    orderedEvent(UsbCdcOrderedType::STREAM_RESET, 3, 6, 1), false);
  CHECK_EQ(static_cast<int>(channel.claim(cursor, delivery)),
           static_cast<int>(UsbCdcOrderedClaimResult::BLOCKED),
           "boundary control waits for its final preceding byte");
  CHECK_EQ(channel.pendingCount(), static_cast<size_t>(1),
           "held control remains visible to available-style polling");
  cursor.noteByteDelivered();
  CHECK_EQ(channel.pendingCount(), static_cast<size_t>(1),
           "newly due held control remains visible after final byte");
  CHECK_EQ(static_cast<int>(channel.claim(cursor, delivery)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "held control is delivered on the next drain iteration");

  UsbCdcOrderedChannel<1> staleChannel;
  cursor.beginSession(5, 0, cursor.epoch());
  staleChannel.publish(
    orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 5, 7, 0), false);
  staleChannel.publish(
    orderedEvent(UsbCdcOrderedType::STREAM_RESET, 4, 8, 0), false);
  CHECK_EQ(static_cast<int>(staleChannel.claim(cursor, delivery)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "current queued control drains before stale fallback");
  cursor.applyControl(delivery.event);
  CHECK_EQ(static_cast<int>(staleChannel.claim(cursor, delivery)),
           static_cast<int>(UsbCdcOrderedClaimResult::STALE_FALLBACK_DISCARDED),
           "stale fallback is explicitly discarded instead of quarantining forever");
  CHECK_EQ(staleChannel.pendingCount(), static_cast<size_t>(0),
           "stale fallback discard clears its slot atomically");
}

static void testSessionGateLinearizesConfigAndTerminalEvents() {
  UsbCdcSessionGate gate;
  gate.startSession(7);
  UsbCdcConfigToken token = gate.configurationToken();
  CHECK_TRUE(gate.noteTerminal(7), "active terminal event is latched");
  CHECK_TRUE(!gate.commitConfiguration(token, true),
             "pending terminal event rejects config commit before queue drain");
  CHECK_TRUE(!gate.callbackEnabled(7),
             "rejected config never enables producer");

  gate.startSession(8);
  token = gate.configurationToken();
  CHECK_TRUE(!gate.noteTerminal(7),
             "prior-session terminal cannot poison new session");
  CHECK_TRUE(gate.commitConfiguration(token, true),
             "unchanged generation commits configuration");
  CHECK_TRUE(gate.callbackEnabled(8), "successful commit enables producer");
  CHECK_TRUE(gate.noteTerminal(8), "terminal after commit is accepted");
  CHECK_TRUE(!gate.callbackEnabled(8),
             "terminal after commit wins and disables producer");
  CHECK_TRUE(!gate.noteTerminal(8), "duplicate terminal is idempotent");

  gate.startSession(9);
  token = gate.configurationToken();
  gate.stopProducer(9);
  CHECK_TRUE(gate.commitConfiguration(token, false),
             "configuration may commit while fallback keeps producer stopped");
  CHECK_TRUE(!gate.callbackEnabled(9), "fallback commit remains quarantined");
  CHECK_TRUE(gate.resumeProducer(9), "active nonterminal session may resume");
  CHECK_TRUE(gate.callbackEnabled(9), "resume re-enables producer once");
}

static void testImmutableContextSlotsAndSessionWrap() {
  UsbCdcSessionSlots<2> slots;
  int first = slots.acquire(11);
  CHECK_TRUE(first >= 0, "first immutable callback slot acquired");
  CHECK_TRUE(slots.matches(static_cast<size_t>(first), 11),
             "active slot validates its immutable session");
  CHECK_TRUE(!slots.release(static_cast<size_t>(first), 12),
             "wrong session cannot retire active context");
  CHECK_TRUE(slots.release(static_cast<size_t>(first), 11),
             "matching session retires context after safe close");
  CHECK_TRUE(!slots.matches(static_cast<size_t>(first), 11),
             "retired context rejects delayed callback");

  CHECK_EQ(usbCdcNextSession(UINT32_MAX), 1U,
           "session generation wraps to nonzero one");
  CHECK_EQ(usbCdcNextSession(0), 1U, "zero is never issued as a session");
}

static void testTeardownRequiresEveryHostMilestone() {
  UsbCdcTeardownTracker teardown(true);
  CHECK_TRUE(!teardown.complete(), "installed driver starts incomplete");
  CHECK_TRUE(!teardown.mayFreeDevices(),
             "devices cannot free before CDC client uninstall");
  teardown.noteCdcUninstalled();
  CHECK_TRUE(!teardown.mayFreeDevices(),
             "NO_CLIENTS must be serviced after CDC uninstall");
  teardown.noteNoClients();
  CHECK_TRUE(teardown.mayFreeDevices(), "NO_CLIENTS permits free-all request");
  teardown.noteFreeAllRequested(false);
  CHECK_TRUE(!teardown.mayUninstallHost(),
             "NOT_FINISHED waits for ALL_FREE host event");
  teardown.noteAllFree();
  CHECK_TRUE(teardown.mayUninstallHost(),
             "all devices and clients gone permits host uninstall");
  teardown.noteHostUninstalled();
  CHECK_TRUE(teardown.complete(), "only successful host uninstall completes");

  UsbCdcTeardownTracker noDriver(false);
  CHECK_TRUE(noDriver.mayFreeDevices(),
             "failed CDC install has no client milestone to await");
}

static void testCloseFailureRetainsOwnershipForRetry() {
  UsbCdcLifecycle state;
  connectReady(state);
  state.apply(event(UsbCdcControlType::TRANSFER_ERROR, -101), 10);
  CHECK_TRUE(state.takeCloseRequest(), "terminal event issues close attempt");
  state.apply(event(UsbCdcControlType::HANDLE_CLOSE_FAILED, -102), 11);
  CHECK_TRUE(state.handleOwned(), "failed close retains handle ownership");
  CHECK_TRUE(state.closePending(), "failed close remains pending");
  CHECK_TRUE(state.takeCloseRequest(), "failed close can be retried");
  state.apply(event(UsbCdcControlType::HANDLE_CLOSED), 12);
  CHECK_TRUE(!state.handleOwned(), "successful retry releases ownership");
}

static void testSynchronizedContextCloseProtocol() {
  UsbCdcSynchronizedState<std::mutex, 2, 2> shared;
  shared.startSession(21);
  int slot = shared.acquireContext(21);
  CHECK_TRUE(slot >= 0, "synchronized context slot acquired");

  {
    auto lease = shared.acquireCallback(static_cast<size_t>(slot), 21);
    CHECK_TRUE(static_cast<bool>(lease), "active callback obtains lease");
    CHECK_TRUE(shared.retireContext(static_cast<size_t>(slot), 21),
               "close retires context before driver dispatch");
    CHECK_EQ(static_cast<int>(shared.finishClose(
               static_cast<size_t>(slot), 21, true)),
             static_cast<int>(UsbCdcCloseCompletion::CALLBACKS_ACTIVE),
             "driver acknowledgement cannot reuse context with active callback");
    CHECK_TRUE(!shared.acquireCallback(static_cast<size_t>(slot), 21),
               "retired context rejects late callback entry");
  }

  CHECK_EQ(static_cast<int>(shared.finishClose(
             static_cast<size_t>(slot), 21, true)),
           static_cast<int>(UsbCdcCloseCompletion::RELEASED),
           "quiescent driver acknowledgement releases context");
  CHECK_TRUE(!shared.acquireCallback(static_cast<size_t>(slot), 21),
             "released context rejects stale callback");

  shared.startSession(22);
  slot = shared.acquireContext(22);
  CHECK_TRUE(slot >= 0, "next session acquires a context");
  shared.retireContext(static_cast<size_t>(slot), 22);
  CHECK_EQ(static_cast<int>(shared.finishClose(
             static_cast<size_t>(slot), 22, false)),
           static_cast<int>(UsbCdcCloseCompletion::DRIVER_FAILED),
           "failed driver close retains immutable context");
  CHECK_TRUE(shared.contextMatches(static_cast<size_t>(slot), 22),
             "failed close cannot reuse active context storage");
  CHECK_EQ(static_cast<int>(shared.finishClose(
             static_cast<size_t>(slot), 22, true)),
           static_cast<int>(UsbCdcCloseCompletion::RELEASED),
           "successful close retry releases retained context");

  shared.startSession(23);
  int reused = shared.acquireContext(23);
  CHECK_TRUE(reused >= 0, "released slot may serve later session");
  CHECK_TRUE(!shared.acquireCallback(static_cast<size_t>(reused), 22),
             "prior-session callback is rejected after slot reuse");
  CHECK_TRUE(shared.acquireCallback(static_cast<size_t>(reused), 23),
             "current immutable context callback is accepted");
}

static void testFailedOpenContextReapsAfterLateCallbackLease() {
  UsbCdcSynchronizedState<std::mutex, 2, 2> shared;
  shared.startSession(24);
  int failedSlot = shared.acquireContext(24);
  CHECK_TRUE(failedSlot >= 0, "failed-open fixture acquires context");

  {
    auto lateCallback = shared.acquireCallback(
      static_cast<size_t>(failedSlot), 24);
    CHECK_TRUE(static_cast<bool>(lateCallback),
               "driver-snapshotted callback owns a shared lease");
    CHECK_TRUE(shared.retireContext(static_cast<size_t>(failedSlot), 24),
               "failed open retires callback admission before detaching");
    CHECK_TRUE(!shared.abandonContext(static_cast<size_t>(failedSlot), 24),
               "first reap keeps a context with a late callback lease");

    shared.startSession(25);
    int nextSlot = shared.acquireContext(25);
    CHECK_TRUE(nextSlot >= 0 && nextSlot != failedSlot,
               "next open uses the other slot while late callback drains");
    CHECK_TRUE(shared.retireContext(static_cast<size_t>(nextSlot), 25),
               "second fixture slot can be retired");
    CHECK_TRUE(shared.abandonContext(static_cast<size_t>(nextSlot), 25),
               "quiescent detached slot reaps immediately");
  }

  CHECK_TRUE(shared.abandonContext(static_cast<size_t>(failedSlot), 24),
             "later poll reaps context after callback lease release");
  shared.startSession(26);
  CHECK_TRUE(shared.acquireContext(26) >= 0,
             "reaped failed-open contexts do not exhaust the slot pool");
}

static void testByteCommitLeaseLinearizesTerminalBoundary() {
  UsbCdcSynchronizedState<std::mutex, 2, 1> shared;
  shared.startSession(30);
  int slot = shared.acquireContext(30);
  CHECK_TRUE(slot >= 0, "byte-commit fixture acquires context");
  UsbCdcConfigToken token = shared.configurationToken();
  CHECK_TRUE(shared.commitConfiguration(
               token, UsbCdcOrderedPublishResult::QUEUED),
             "byte-commit fixture enables producer");

  {
    auto callback = shared.acquireCallback(static_cast<size_t>(slot), 30);
    CHECK_TRUE(static_cast<bool>(callback),
               "active callback obtains callback lease");
    {
      UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
      auto commit = shared.acquireByteCommitOrRecordDrop(
        static_cast<size_t>(slot), 30, 0, 0, admission);
      CHECK_TRUE(static_cast<bool>(commit),
                 "enabled callback obtains byte-commit lease");
      CHECK_EQ(static_cast<int>(admission),
               static_cast<int>(UsbCdcByteAdmission::ADMITTED),
               "successful byte admission is explicit");
      CHECK_TRUE(shared.latchTerminal(30),
                 "terminal latch stops future byte admission");
      CHECK_TRUE(!shared.callbacksQuiescent(
                   static_cast<size_t>(slot), 30),
                 "terminal boundary waits for admitted byte commit");
    }
    UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
    auto rejected = shared.acquireByteCommitOrRecordDrop(
      static_cast<size_t>(slot), 30, 0, 4, admission);
    CHECK_TRUE(!rejected,
               "terminal latch rejects a later byte commit");
    CHECK_EQ(static_cast<int>(admission),
             static_cast<int>(UsbCdcByteAdmission::REJECTED_TERMINAL),
             "terminal rejection is distinct from overflow aggregation");
    CHECK_TRUE(!shared.callbacksQuiescent(static_cast<size_t>(slot), 30),
               "terminal boundary also waits for callback lease release");
  }
  CHECK_TRUE(shared.callbacksQuiescent(static_cast<size_t>(slot), 30),
             "terminal boundary may snapshot only after both leases release");
}

static void testOverflowQuarantineAggregatesAndResumes() {
  UsbCdcSynchronizedState<std::mutex, 2, 1> queued;
  queued.startSession(31);
  int slot = queued.acquireContext(31);
  UsbCdcConfigToken token = queued.configurationToken();
  CHECK_TRUE(queued.commitConfiguration(
               token, UsbCdcOrderedPublishResult::QUEUED),
             "queued overflow fixture enables producer");

  {
    auto callback = queued.acquireCallback(static_cast<size_t>(slot), 31);
    UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
    auto commit = queued.acquireByteCommitOrRecordDrop(
      static_cast<size_t>(slot), 31, 4, 0, admission);
    CHECK_TRUE(static_cast<bool>(commit),
               "first callback is admitted before partial write");
    CHECK_EQ(static_cast<int>(queued.publishQuarantineBarrier(
               orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 31, 4, 5, 3))),
             static_cast<int>(UsbCdcOrderedPublishResult::QUEUED),
             "partial write publishes a queued recovery marker");
  }

  {
    auto callback = queued.acquireCallback(static_cast<size_t>(slot), 31);
    UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
    auto commit = queued.acquireByteCommitOrRecordDrop(
      static_cast<size_t>(slot), 31, 4, 7, admission);
    CHECK_TRUE(!commit,
               "later bytes stay quarantined before queued marker drains");
    CHECK_EQ(static_cast<int>(admission),
             static_cast<int>(UsbCdcByteAdmission::DROP_RECORDED),
             "rejection and queued-marker aggregation are one operation");
  }

  UsbCdcOrderedCursor cursor;
  cursor.beginSession(31, 5, 0);
  UsbCdcOrderedDelivery delivery;
  CHECK_EQ(static_cast<int>(queued.claim(cursor, delivery, true)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "queued recovery marker drains at its byte boundary");
  CHECK_TRUE(delivery.producerResumed,
             "queued recovery reports actual producer resume");
  CHECK_EQ(delivery.event.droppedBytes, 10U,
           "queued marker preserves exact accumulated dropped bytes");
  UsbCdcLifecycle lifecycle;
  connectReady(lifecycle);
  lifecycle.apply(event(UsbCdcControlType::RX_OVERFLOW, 0,
                        delivery.event.droppedBytes), 0);
  lifecycle.apply(event(UsbCdcControlType::RX_CAPACITY_RECOVERED), 0);
  CHECK_EQ(lifecycle.droppedBytes(), 10U,
           "authoritative marker counts partial and quarantined drops once");
  CHECK_EQ(lifecycle.overflowEpisodes(), 1U,
           "authoritative marker counts one recovered overflow episode");
  {
    auto callback = queued.acquireCallback(static_cast<size_t>(slot), 31);
    UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
    CHECK_TRUE(queued.acquireByteCommitOrRecordDrop(
                 static_cast<size_t>(slot), 31, 4, 0, admission),
               "queued recovery marker resumes byte admission");
  }

  UsbCdcSynchronizedState<std::mutex, 1, 1> fallback;
  fallback.startSession(32);
  slot = fallback.acquireContext(32);
  token = fallback.configurationToken();
  CHECK_TRUE(fallback.commitConfiguration(
               token, UsbCdcOrderedPublishResult::QUEUED),
             "fallback overflow fixture enables producer");
  fallback.publish(
    orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 32, 1, 0, 1), false);
  CHECK_EQ(static_cast<int>(fallback.publishQuarantineBarrier(
             orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 32, 2, 0, 2))),
           static_cast<int>(UsbCdcOrderedPublishResult::FALLBACK_CREATED),
           "full channel stores overflow recovery in fallback");
  {
    auto callback = fallback.acquireCallback(static_cast<size_t>(slot), 32);
    UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
    auto commit = fallback.acquireByteCommitOrRecordDrop(
      static_cast<size_t>(slot), 32, 2, 5, admission);
    CHECK_TRUE(!commit, "fallback quarantine rejects later byte commit");
    CHECK_EQ(static_cast<int>(admission),
             static_cast<int>(UsbCdcByteAdmission::DROP_RECORDED),
             "fallback rejection atomically aggregates its dropped bytes");
  }

  cursor.beginSession(32, 0, 0);
  CHECK_EQ(static_cast<int>(fallback.claim(cursor, delivery, true)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "older queued control drains before fallback");
  CHECK_TRUE(!delivery.producerResumed,
             "older queued control does not report premature resume");
  cursor.applyControl(delivery.event);
  CHECK_TRUE(fallback.quarantined(),
             "older queued control cannot resume fallback quarantine");
  CHECK_EQ(static_cast<int>(fallback.claim(cursor, delivery, true)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "fallback recovery marker drains second");
  CHECK_TRUE(delivery.producerResumed,
             "fallback recovery reports actual producer resume");
  CHECK_EQ(delivery.event.droppedBytes, 7U,
           "fallback marker preserves exact accumulated dropped bytes");
  {
    auto callback = fallback.acquireCallback(static_cast<size_t>(slot), 32);
    UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
    CHECK_TRUE(fallback.acquireByteCommitOrRecordDrop(
                 static_cast<size_t>(slot), 32, 2, 0, admission),
               "fallback recovery marker resumes byte admission");
  }
}

static void testConfigurationHandleGuard() {
  CHECK_EQ(usbCdcEffectiveControlSession(
             UsbCdcControlType::CONTROL_QUEUE_OVERFLOW, 0, 39), 39U,
           "global control failure is normalized to active session");
  CHECK_EQ(usbCdcEffectiveControlSession(
             UsbCdcControlType::CONTROL_QUEUE_OVERFLOW, 38, 39), 38U,
           "explicit control failure session is preserved");
  CHECK_EQ(usbCdcEffectiveControlSession(
             UsbCdcControlType::TRANSFER_ERROR, 0, 39), 0U,
           "session-scoped callback facts are never silently rebound");
  CHECK_TRUE(usbCdcConfigurationMayContinue(
               UsbCdcPhase::CONFIGURING, false, true, true, true),
             "valid configuring handle may be used");
  CHECK_TRUE(!usbCdcConfigurationMayContinue(
               UsbCdcPhase::READY, false, true, true, true),
             "non-configuring lifecycle rejects handle operation");
  CHECK_TRUE(!usbCdcConfigurationMayContinue(
               UsbCdcPhase::CONFIGURING, true, true, true, true),
             "pending close rejects handle operation");
  CHECK_TRUE(!usbCdcConfigurationMayContinue(
               UsbCdcPhase::CONFIGURING, false, false, true, true),
             "replaced handle rejects stale candidate operation");
  CHECK_TRUE(!usbCdcConfigurationMayContinue(
               UsbCdcPhase::CONFIGURING, false, true, false, true),
             "retired callback context rejects handle operation");
  CHECK_TRUE(!usbCdcConfigurationMayContinue(
               UsbCdcPhase::CONFIGURING, false, true, true, false),
             "superseded configuration token rejects handle operation");

  UsbCdcSynchronizedState<std::mutex, 2, 1> shared;
  shared.startSession(40);
  int slot = shared.acquireContext(40);
  UsbCdcConfigToken token = shared.configurationToken();
  CHECK_TRUE(shared.configurationTokenValid(token),
             "current configuration token validates before handle use");
  CHECK_TRUE(shared.configurationContextValid(static_cast<size_t>(slot), 40),
             "current configuration context validates before handle use");
  shared.latchTerminal(40);
  CHECK_TRUE(!shared.configurationTokenValid(token),
             "terminal generation invalidates the captured config token");
}

static void testStartResetCannotReleaseLaterOverflowBarrier() {
  UsbCdcSynchronizedState<std::mutex, 3, 1> shared;
  shared.startSession(41);
  int slot = shared.acquireContext(41);
  UsbCdcConfigToken token = shared.configurationToken();
  CHECK_TRUE(shared.commitConfiguration(
               token, UsbCdcOrderedPublishResult::QUEUED),
             "start-reset ordering fixture enables producer");
  shared.publish(
    orderedEvent(UsbCdcOrderedType::STREAM_RESET, 41, 2, 0), true);
  shared.publishQuarantineBarrier(
    orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 41, 2, 0, 2));

  {
    auto callback = shared.acquireCallback(static_cast<size_t>(slot), 41);
    UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
    CHECK_TRUE(!shared.acquireByteCommitOrRecordDrop(
                 static_cast<size_t>(slot), 41, 2, 5, admission),
               "later overflow bytes remain rejected");
    CHECK_EQ(static_cast<int>(admission),
             static_cast<int>(UsbCdcByteAdmission::DROP_RECORDED),
             "later drops attach to the overflow barrier, not start reset");
  }

  UsbCdcOrderedCursor cursor;
  cursor.beginSession(41, 0, 0);
  UsbCdcOrderedDelivery delivery;
  CHECK_EQ(static_cast<int>(shared.claim(cursor, delivery, true)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "initial stream reset drains first");
  CHECK_TRUE(!delivery.producerResumed,
             "start reset cannot report overflow-barrier resume");
  CHECK_EQ(delivery.event.droppedBytes, 0U,
           "initial stream reset does not absorb later overflow drops");
  CHECK_TRUE(shared.quarantined(),
             "initial stream reset cannot release later overflow barrier");
  cursor.applyControl(delivery.event);
  CHECK_EQ(static_cast<int>(shared.claim(cursor, delivery, true)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "overflow barrier drains second");
  CHECK_TRUE(delivery.producerResumed,
             "overflow barrier reports final producer resume");
  CHECK_EQ(delivery.event.droppedBytes, 7U,
           "overflow barrier owns every quarantined drop");
  CHECK_TRUE(!shared.quarantined(),
             "only overflow barrier release resumes producer");
}

static void testTerminalDominatedFallbackStillCarriesDrops() {
  UsbCdcSynchronizedState<std::mutex, 1, 1> shared;
  shared.startSession(42);
  int slot = shared.acquireContext(42);
  UsbCdcConfigToken token = shared.configurationToken();
  CHECK_TRUE(slot >= 0 && shared.commitConfiguration(
               token, UsbCdcOrderedPublishResult::QUEUED),
             "terminal fallback fixture enables producer");
  shared.publish(
    orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 42, 1, 0), false);
  shared.publishQuarantineBarrier(
    orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 42, 2, 0, 3));
  CHECK_TRUE(shared.latchTerminal(42),
             "terminal event stops fallback recovery");
  shared.publish(
    orderedEvent(UsbCdcOrderedType::STREAM_RESET, 42, 3, 0, 4), false);

  UsbCdcOrderedCursor cursor;
  cursor.beginSession(42, 0, 0);
  UsbCdcOrderedDelivery delivery;
  CHECK_EQ(static_cast<int>(shared.claim(cursor, delivery, true)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "queued predecessor drains before terminal fallback");
  cursor.applyControl(delivery.event);
  CHECK_EQ(static_cast<int>(shared.claim(cursor, delivery, true)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "terminal-dominated fallback remains deliverable");
  CHECK_EQ(static_cast<int>(delivery.event.type),
           static_cast<int>(UsbCdcOrderedType::STREAM_RESET),
           "terminal reset dominates fallback recovery marker");
  CHECK_EQ(delivery.event.droppedBytes, 7U,
           "terminal fallback preserves every overflow drop");
  CHECK_TRUE(!delivery.producerResumed,
             "terminal-dominated fallback never reports producer resume");

  UsbCdcLifecycle lifecycle;
  connectReady(lifecycle);
  lifecycle.apply(event(UsbCdcControlType::RX_OVERFLOW, 0,
                        delivery.event.droppedBytes), 0);
  CHECK_EQ(lifecycle.droppedBytes(), 7U,
           "main owner counts terminal-dominated delivery drops once");
}

static void testTwoQueuedOverflowBarriersResumeOnlyAfterLast() {
  UsbCdcSynchronizedState<std::mutex, 3, 1> shared;
  shared.startSession(43);
  int slot = shared.acquireContext(43);
  UsbCdcConfigToken token = shared.configurationToken();
  CHECK_TRUE(slot >= 0 && shared.commitConfiguration(
               token, UsbCdcOrderedPublishResult::QUEUED),
             "two-barrier fixture enables producer");
  shared.publishQuarantineBarrier(
    orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 43, 1, 0, 2));
  shared.publishQuarantineBarrier(
    orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 43, 2, 0, 3));

  UsbCdcOrderedCursor cursor;
  cursor.beginSession(43, 0, 0);
  UsbCdcOrderedDelivery delivery;
  CHECK_EQ(static_cast<int>(shared.claim(cursor, delivery, true)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "first queued overflow barrier drains first");
  CHECK_TRUE(!delivery.producerResumed && shared.quarantined(),
             "first barrier cannot resume while second barrier is queued");
  cursor.applyControl(delivery.event);

  {
    auto callback = shared.acquireCallback(static_cast<size_t>(slot), 43);
    UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
    CHECK_TRUE(!shared.acquireByteCommitOrRecordDrop(
                 static_cast<size_t>(slot), 43, 2, 5, admission),
               "bytes between queued barriers remain rejected");
    CHECK_EQ(static_cast<int>(admission),
             static_cast<int>(UsbCdcByteAdmission::DROP_RECORDED),
             "inter-barrier drops attach to the last queued barrier");
  }

  CHECK_EQ(static_cast<int>(shared.claim(cursor, delivery, true)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "last queued overflow barrier drains second");
  CHECK_EQ(delivery.event.droppedBytes, 8U,
           "last queued barrier owns later rejected bytes");
  CHECK_TRUE(delivery.producerResumed && !shared.quarantined(),
             "only last queued barrier resumes producer");
}

static void testTerminalFactsUpgradeErrorToDisconnect() {
  UsbCdcSynchronizedState<std::mutex, 3, 1> shared;
  shared.startSession(44);
  int slot = shared.acquireContext(44);
  CHECK_TRUE(slot >= 0, "terminal-fact fixture acquires context");
  CHECK_EQ(static_cast<int>(shared.noteTerminalFact(
             44, UsbCdcTerminalFact::ERROR)),
           static_cast<int>(UsbCdcTerminalUpdate::FIRST),
           "first transfer error publishes terminal fact");
  CHECK_EQ(static_cast<int>(shared.noteTerminalFact(
             44, UsbCdcTerminalFact::ERROR)),
           static_cast<int>(UsbCdcTerminalUpdate::IGNORED),
           "duplicate transfer error is suppressed");
  CHECK_EQ(static_cast<int>(shared.noteTerminalFact(
             44, UsbCdcTerminalFact::DISCONNECTED)),
           static_cast<int>(UsbCdcTerminalUpdate::UPGRADED),
           "disconnect upgrades an earlier transfer error");
  CHECK_EQ(static_cast<int>(shared.noteTerminalFact(
             44, UsbCdcTerminalFact::DISCONNECTED)),
           static_cast<int>(UsbCdcTerminalUpdate::IGNORED),
           "duplicate disconnect is suppressed");

  CHECK_TRUE(shared.retireContext(static_cast<size_t>(slot), 44),
             "terminal owner retires normal callback admission");
  CHECK_TRUE(!shared.acquireCallback(static_cast<size_t>(slot), 44),
             "retiring context rejects ordinary callbacks");
  CHECK_TRUE(shared.acquireTerminalCallback(static_cast<size_t>(slot), 44),
             "retiring context still admits terminal fact callback");

  UsbCdcSynchronizedState<std::mutex, 3, 1> disconnectFirst;
  disconnectFirst.startSession(45);
  CHECK_EQ(static_cast<int>(disconnectFirst.noteTerminalFact(
             45, UsbCdcTerminalFact::DISCONNECTED)),
           static_cast<int>(UsbCdcTerminalUpdate::FIRST),
           "disconnect-first publishes strongest terminal fact");
  CHECK_EQ(static_cast<int>(disconnectFirst.noteTerminalFact(
             45, UsbCdcTerminalFact::ERROR)),
           static_cast<int>(UsbCdcTerminalUpdate::IGNORED),
           "later transfer error cannot downgrade disconnect");

  UsbCdcTerminalBoundaryTracker boundary;
  CHECK_TRUE(boundary.needsPublish(UsbCdcOrderedType::DISCONTINUITY),
             "first error needs discontinuity boundary");
  boundary.notePublished(UsbCdcOrderedType::DISCONTINUITY);
  CHECK_TRUE(!boundary.needsPublish(UsbCdcOrderedType::DISCONTINUITY),
             "duplicate error does not republish boundary");
  CHECK_TRUE(boundary.needsPublish(UsbCdcOrderedType::STREAM_RESET),
             "disconnect upgrades published discontinuity to reset");
  boundary.notePublished(UsbCdcOrderedType::STREAM_RESET);
  CHECK_TRUE(!boundary.needsPublish(UsbCdcOrderedType::DISCONTINUITY),
             "error cannot downgrade published reset");
  boundary.reset();
  CHECK_TRUE(boundary.needsPublish(UsbCdcOrderedType::DISCONTINUITY),
             "new session resets terminal boundary severity");
}

static void testSynchronizedDiagnosticsAreExactAndConsistent() {
  UsbCdcSynchronizedState<std::mutex, 3, 1> shared;
  shared.startSession(46);
  int slot = shared.acquireContext(46);
  CHECK_TRUE(slot >= 0, "diagnostics fixture acquires context");
  UsbCdcConfigToken token = shared.configurationToken();
  CHECK_TRUE(shared.commitConfiguration(
               token, UsbCdcOrderedPublishResult::QUEUED),
             "diagnostics fixture enables producer");

  shared.recordRejectedDrop(7);
  UsbCdcDiagnosticsSnapshot snapshot = shared.diagnosticsSnapshot();
  CHECK_EQ(snapshot.droppedBytes, 7U,
           "rejected late bytes are counted without callback admission");
  CHECK_EQ(snapshot.lossEpisodes, 0U,
           "rejected bytes do not invent an unordered loss boundary");

  uint32_t epoch = shared.beginOverflowLoss(5);
  snapshot = shared.diagnosticsSnapshot();
  CHECK_EQ(epoch, 1U, "first overflow advances producer epoch");
  CHECK_EQ(snapshot.droppedBytes, 12U,
           "overflow and rejected drops share one exact counter");
  CHECK_EQ(snapshot.lossEpisodes, 1U, "overflow starts one loss episode");
  CHECK_EQ(snapshot.overflowEpisodes, 1U,
           "overflow episode is counted once");

  shared.publishQuarantineBarrier(
    orderedEvent(UsbCdcOrderedType::DISCONTINUITY, 46, epoch, 0, 5));
  UsbCdcOrderedCursor cursor;
  cursor.beginSession(46, 0, 0);
  UsbCdcOrderedDelivery delivery;
  CHECK_EQ(static_cast<int>(shared.claim(cursor, delivery, true)),
           static_cast<int>(UsbCdcOrderedClaimResult::CLAIMED),
           "overflow recovery barrier is claimed");
  CHECK_TRUE(delivery.producerResumed,
             "claim closes the synchronized overflow episode");

  CHECK_EQ(shared.beginOverflowLoss(3), 2U,
           "overflow after recovery begins a new epoch");
  CHECK_EQ(shared.beginTerminalLoss(), 3U,
           "terminal loss advances epoch once");
  CHECK_EQ(shared.beginTerminalLoss(), 3U,
           "duplicate terminal loss reuses the same epoch");
  snapshot = shared.diagnosticsSnapshot();
  CHECK_EQ(snapshot.droppedBytes, 15U,
           "diagnostic snapshot contains all exact drops");
  CHECK_EQ(snapshot.lossEpisodes, 3U,
           "two overflows plus terminal loss are consistent");
  CHECK_EQ(snapshot.overflowEpisodes, 2U,
           "recovered overflow can start a new episode");
  CHECK_EQ(snapshot.producerEpoch, 3U,
           "snapshot epoch matches its loss counters");

  shared.recordRejectedDrop(UINT32_MAX);
  CHECK_EQ(shared.diagnosticsSnapshot().droppedBytes, UINT32_MAX,
           "diagnostic drop counter saturates");
}

static void testQueueSaturationReplaysStrongestTerminalFact() {
  UsbCdcSynchronizedState<std::mutex, 3, 1> shared;
  shared.startSession(47);
  CHECK_EQ(static_cast<int>(shared.noteTerminalFact(
             47, UsbCdcTerminalFact::ERROR)),
           static_cast<int>(UsbCdcTerminalUpdate::FIRST),
           "failed error enqueue leaves a pending terminal fact");
  CHECK_EQ(static_cast<int>(shared.pendingTerminalFact(47)),
           static_cast<int>(UsbCdcTerminalFact::ERROR),
           "pending terminal replay starts at error severity");
  CHECK_EQ(static_cast<int>(shared.noteTerminalFact(
             47, UsbCdcTerminalFact::DISCONNECTED)),
           static_cast<int>(UsbCdcTerminalUpdate::UPGRADED),
           "failed disconnect enqueue upgrades pending severity");
  shared.acknowledgeTerminalFact(47, UsbCdcTerminalFact::ERROR);
  CHECK_EQ(static_cast<int>(shared.pendingTerminalFact(47)),
           static_cast<int>(UsbCdcTerminalFact::DISCONNECTED),
           "weaker acknowledgement cannot erase pending disconnect");

  UsbCdcLifecycle lifecycle;
  connectReady(lifecycle);
  lifecycle.apply(event(UsbCdcControlType::CONTROL_QUEUE_OVERFLOW), 1);
  CHECK_TRUE(lifecycle.deviceAttached(),
             "generic saturation alone cannot prove physical disconnect");
  UsbCdcTerminalFact pending = shared.pendingTerminalFact(47);
  CHECK_EQ(static_cast<int>(pending),
           static_cast<int>(UsbCdcTerminalFact::DISCONNECTED),
           "drain observes strongest typed terminal fact");
  if (pending == UsbCdcTerminalFact::DISCONNECTED) {
    lifecycle.apply(event(UsbCdcControlType::DEVICE_DISCONNECTED), 1);
  }
  CHECK_TRUE(!lifecycle.deviceAttached(),
             "typed replay preserves disconnect attachment semantics");
  shared.acknowledgeTerminalFact(47, pending);
  CHECK_EQ(static_cast<int>(shared.pendingTerminalFact(47)),
           static_cast<int>(UsbCdcTerminalFact::NONE),
           "successful typed replay acknowledges exactly its severity");
}

static void testCdcNotificationParserRejectsShortPayloads() {
  CdcAcmNotificationView view;
  const uint8_t shortHeader[7] = {};
  CHECK_TRUE(!cdc_acm_notification_parse(shortHeader, sizeof(shortHeader),
                                          &view),
             "notification shorter than fixed header is rejected");

  const uint8_t truncatedPayload[8] = {
    0xA1, USB_CDC_NOTIF_SERIAL_STATE, 0, 0, 0, 0, 2, 0,
  };
  CHECK_TRUE(!cdc_acm_notification_parse(
               truncatedPayload, sizeof(truncatedPayload), &view),
             "declared payload must fit actual transfer length");

  const uint8_t shortSerialState[9] = {
    0xA1, USB_CDC_NOTIF_SERIAL_STATE, 0, 0, 0, 0, 1, 0, 0x01,
  };
  CHECK_TRUE(!cdc_acm_notification_parse(
               shortSerialState, sizeof(shortSerialState), &view),
             "serial-state notification requires two payload bytes");

  const uint8_t emptySerialState[8] = {
    0xA1, USB_CDC_NOTIF_SERIAL_STATE, 0, 0, 0, 0, 0, 0,
  };
  CHECK_TRUE(!cdc_acm_notification_parse(
               emptySerialState, sizeof(emptySerialState), &view),
             "serial-state notification rejects an empty payload");

  const uint8_t oversizedSerialState[11] = {
    0xA1, USB_CDC_NOTIF_SERIAL_STATE, 0, 0, 0, 0, 3, 0,
    0x34, 0x12, 0xFF,
  };
  CHECK_TRUE(!cdc_acm_notification_parse(
               oversizedSerialState, sizeof(oversizedSerialState), &view),
             "serial-state notification requires exactly two bytes");

  const uint8_t validSerialState[10] = {
    0xA1, USB_CDC_NOTIF_SERIAL_STATE, 0, 0, 0, 0, 2, 0, 0x34, 0x12,
  };
  CHECK_TRUE(cdc_acm_notification_parse(
               validSerialState, sizeof(validSerialState), &view),
             "complete serial-state notification parses");
  CHECK_EQ(view.serialState, 0x1234U,
           "serial state is decoded from USB little-endian bytes");

  const uint8_t networkConnection[8] = {
    0xA1, USB_CDC_NOTIF_NETWORK_CONNECTION, 1, 0, 0, 0, 0, 0,
  };
  CHECK_TRUE(cdc_acm_notification_parse(
               networkConnection, sizeof(networkConnection), &view),
             "header-only network connection notification parses");
  CHECK_EQ(view.value, 1U,
           "notification value is decoded without packed dereference");
  CHECK_EQ(view.interfaceIndex, 0U,
           "notification interface index is decoded little-endian");

  const uint8_t wrongRequestType[8] = {
    0x21, USB_CDC_NOTIF_NETWORK_CONNECTION, 1, 0, 0, 0, 0, 0,
  };
  CHECK_TRUE(!cdc_acm_notification_parse(
               wrongRequestType, sizeof(wrongRequestType), &view),
             "notification must be class/interface device-to-host data");

  const uint8_t networkWithPayload[9] = {
    0xA1, USB_CDC_NOTIF_NETWORK_CONNECTION, 1, 0, 0, 0, 1, 0, 0,
  };
  CHECK_TRUE(!cdc_acm_notification_parse(
               networkWithPayload, sizeof(networkWithPayload), &view),
             "network-connection notification requires zero payload bytes");

  uint8_t unalignedStorage[11] = {};
  for (size_t i = 0; i < sizeof(validSerialState); ++i) {
    unalignedStorage[i + 1] = validSerialState[i];
  }
  CHECK_TRUE(cdc_acm_notification_parse(
               &unalignedStorage[1], sizeof(validSerialState), &view),
             "notification parser accepts an unaligned byte buffer safely");
  CHECK_EQ(view.serialState, 0x1234U,
           "unaligned serial-state decode remains little-endian");
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
  testOrderedChannelFallbackIsAtomicAndOrdered();
  testSessionGateLinearizesConfigAndTerminalEvents();
  testImmutableContextSlotsAndSessionWrap();
  testTeardownRequiresEveryHostMilestone();
  testCloseFailureRetainsOwnershipForRetry();
  testSynchronizedContextCloseProtocol();
  testFailedOpenContextReapsAfterLateCallbackLease();
  testByteCommitLeaseLinearizesTerminalBoundary();
  testOverflowQuarantineAggregatesAndResumes();
  testConfigurationHandleGuard();
  testStartResetCannotReleaseLaterOverflowBarrier();
  testTerminalDominatedFallbackStillCarriesDrops();
  testTwoQueuedOverflowBarriersResumeOnlyAfterLast();
  testTerminalFactsUpgradeErrorToDisconnect();
  testSynchronizedDiagnosticsAreExactAndConsistent();
  testQueueSaturationReplaysStrongestTerminalFact();
  testCdcNotificationParserRejectsShortPayloads();
  return testReport();
}
