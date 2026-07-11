#include "lib/NetworkLifecycle.h"
#include "test_support.h"

#include <cstdint>

using namespace bp_network;

static void testProvisioningAndStaLossPolicy() {
  NetworkLifecycle lifecycle;
  lifecycle.startProvisioning();
  CHECK_EQ(static_cast<int>(lifecycle.phase()),
           static_cast<int>(NetworkPhase::PROVISIONING_AP),
           "unclaimed commissioning explicitly opens provisioning AP");
  CHECK_TRUE(lifecycle.apRequired(),
             "provisioning phase requires AP");

  lifecycle.startStaOnly();
  CHECK_EQ(static_cast<int>(lifecycle.phase()),
           static_cast<int>(NetworkPhase::STA_ONLY),
           "claimed configured boot starts STA only");
  lifecycle.observeStaConnected(true);
  lifecycle.observeStaConnected(false);
  CHECK_EQ(static_cast<int>(lifecycle.phase()),
           static_cast<int>(NetworkPhase::STA_ONLY),
           "transient STA loss never reopens AP");
  CHECK_TRUE(!lifecycle.apRequired(),
             "STA reconnect remains background-only");
}

static void testRecoveryRequiresPresenceAndExpires() {
  NetworkLifecycle lifecycle;
  lifecycle.startStaOnly();
  CHECK_TRUE(!lifecycle.beginRecovery(false, true, 100),
             "remote request cannot open recovery AP");
  CHECK_EQ(static_cast<int>(lifecycle.phase()),
           static_cast<int>(NetworkPhase::STA_ONLY),
           "denied recovery preserves prior mode");

  CHECK_TRUE(lifecycle.beginRecovery(true, true, 100),
             "physical presence opens bounded recovery");
  CHECK_EQ(static_cast<int>(lifecycle.phase()),
           static_cast<int>(NetworkPhase::RECOVERY_AP),
           "recovery phase is explicit");
  CHECK_TRUE(!lifecycle.tick(100 + NetworkLifecycle::kRecoveryWindowMs - 1),
             "recovery remains open before exact deadline");
  CHECK_TRUE(lifecycle.tick(100 + NetworkLifecycle::kRecoveryWindowMs),
             "recovery closes exactly at deadline");
  CHECK_EQ(static_cast<int>(lifecycle.phase()),
           static_cast<int>(NetworkPhase::STA_ONLY),
           "configured recovery returns to STA-only mode");
}

static void testLockedRecoveryAndClockWrap() {
  NetworkLifecycle lifecycle;
  lifecycle.startLocked();
  const uint32_t start = UINT32_MAX - 100U;
  CHECK_TRUE(lifecycle.beginRecovery(true, false, start),
             "physical recovery can service claimed device without WiFi");
  CHECK_TRUE(!lifecycle.tick(start +
                              NetworkLifecycle::kRecoveryWindowMs - 1U),
             "wrapped recovery clock remains active before deadline");
  CHECK_TRUE(lifecycle.tick(start + NetworkLifecycle::kRecoveryWindowMs),
             "wrapped recovery closes at exact deadline");
  CHECK_EQ(static_cast<int>(lifecycle.phase()),
           static_cast<int>(NetworkPhase::LOCKED),
           "credentialless recovery returns to network-locked state");
}

int main() {
  testProvisioningAndStaLossPolicy();
  testRecoveryRequiresPresenceAndExpires();
  testLockedRecoveryAndClockWrap();
  return testReport();
}
