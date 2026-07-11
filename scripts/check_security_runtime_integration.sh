#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

sketch=BP_checker.ino
handler=lib/WebHandler.h
wifi=lib/WiFiManager.h
policy=lib/WebAccessPolicy.h
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

grep -Fq '#include "lib/BoundedWebServer.h"' "$sketch"
grep -Fq '#include "lib/DeviceSecurity.h"' "$sketch"
grep -Fq 'bp_web::BoundedWebServer server(80);' "$sketch"
grep -Fq 'DeviceSecurity deviceSecurity' "$sketch"
grep -Fq 'server.configureAccess(' "$sketch"
grep -Fq 'bootloader_random_enable();' "$sketch"
grep -Fq 'bootloader_random_disable();' "$sketch"
grep -Fq 'esp_fill_random(' "$sketch"
for token in \
  '#include "lib/FirmwareUpdateRuntime.h"' \
  'FirmwareUpdateRuntime firmwareUpdateRuntime' \
  'firmwareUpdateRuntime.begin(' \
  'server.configureStreamConsumer(firmwareUpdateRuntime.streamCallbacks())' \
  'firmwareUpdateRuntime.confirmPendingBoot(' \
  'firmwareUpdateRuntime.rollbackIfPending()'
do
  grep -Fq -- "$token" "$sketch" || {
    echo "missing firmware update boot integration: $token" >&2
    exit 1
  }
done

if grep -Eq '(^|[^A-Za-z])WebServer server\(' "$sketch"; then
  echo "stock WebServer remains the product runtime" >&2
  exit 1
fi
if grep -Fq '12345678' "$sketch" "$handler" "$wifi"; then
  echo "repository-default AP credential remains reachable" >&2
  exit 1
fi

grep -Fq 'WiFi.persistent(false);' "$sketch"
persistent_line=$(grep -nF 'WiFi.persistent(false);' "$sketch" | head -1 | cut -d: -f1)
first_runtime_wifi_line=$(grep -nE 'erasePendingExternalState\(\)|wifiManager->(startProvisioningAP|connectToWiFi|startRecoveryMode)' \
  "$sketch" | awk -F: -v p="$persistent_line" '$1 > p {print $1; exit}')
if [[ -z "$first_runtime_wifi_line" || ! "$persistent_line" -lt "$first_runtime_wifi_line" ]]; then
  echo "WiFi.persistent(false) must precede first radio initialization" >&2
  exit 1
fi
if grep -Fq 'WiFi.persistent(true)' "$sketch" "$wifi"; then
  echo "driver persistence must remain disabled" >&2
  exit 1
fi

rg -o 'server->on\("[^"]+", HTTP_(GET|POST)' "$handler" \
  | sed -E 's/.*server->on\("([^"]+)", HTTP_(GET|POST)/\2 \1/' \
  | sort > "$tmp/handlers"
rg -o '\{HttpMethod::(GET|POST),[[:space:]]*"[^"]+"' "$policy" \
  | sed -E 's/\{HttpMethod::(GET|POST),[[:space:]]*"([^"]+)"/\1 \2/' \
  | sort > "$tmp/policy"
if ! diff -u "$tmp/policy" "$tmp/handlers"; then
  echo "handler routes and default-deny registry differ" >&2
  exit 1
fi

for token in \
  'BoundedWebServer* server' \
  'DeviceSecurity* deviceSecurity' \
  'server->on("/claim", HTTP_GET' \
  'server->on("/claim", HTTP_POST' \
  'server->on("/security", HTTP_GET' \
  'server->on("/rotate_credentials", HTTP_POST' \
  'server->recordClaimResult(' \
  'deviceSecurity->recoverWithBootstrap(' \
  'server->deferAfterResponse(' \
  'deviceSecurity->requestWipe(DeviceWipeKind::NETWORK)' \
  'isProductionModelAllowed('
do
  grep -Fq -- "$token" "$handler" || {
    echo "missing secure handler integration: $token" >&2
    exit 1
  }
done

if grep -Eq '/set_pin|adminPin|pinBlocked|renderPinField|delay\(' "$handler"; then
  echo "legacy PIN or blocking authentication path remains" >&2
  exit 1
fi
grep -Fq 'String observedPassword;' "$handler"
grep -Fq 'secureWipeString(observedPassword);' "$handler"
grep -Fq 'putString("password", new_password.c_str())' "$handler"
if grep -Fq 'getString("password", "") == new_password' "$handler"; then
  echo "temporary password copy is not securely wiped" >&2
  exit 1
fi

grep -Fq 'isProductionModelAllowed(storedModel.c_str())' "$sketch"
grep -Fq 'credentialRotationRequiresRestart(' "$handler"
grep -Fq 'AP 密碼已輪替；回應完成後裝置將重新啟動' "$handler"
grep -Fq '復原 AP 密碼' "$handler"
grep -Fq 'deviceSecurity->tokenConsumed()' "$handler"
grep -Fq '部署前請先建立實體復原碼' "$handler"
if grep -Fq '若接入其他格式可選擇自定義' "$handler"; then
  echo "production UI still advertises an unsupported model" >&2
  exit 1
fi

grep -Fq 'finishExternalErase(' "$sketch"
grep -Fq 'WiFi.eraseAP()' "$sketch"
grep -Fq 'WiFi.softAPdisconnect(true)' "$wifi"
grep -Fq 'bp_network::NetworkLifecycle lifecycle' "$wifi"
grep -Fq 'lifecycle.beginRecovery(true, hasCredentials(), nowMs)' "$wifi"
grep -Fq 'lifecycle.observeStaConnected(nowConnected)' "$wifi"
grep -Fq 'void discardLoadedCredentials()' "$wifi"
grep -Fq 'wifiManager->discardLoadedCredentials();' "$sketch"
if ! awk '/WiFi.begin\(staSsid.c_str\(\), staPassword.c_str\(\)\)/ { seen=1; next }
         seen && /secureWipeString\(staPassword\)/ { found=1; exit }
         END { exit found ? 0 : 1 }' "$wifi"; then
  echo "STA password remains in application RAM after WiFi.begin" >&2
  exit 1
fi

echo "security runtime integration contract passed"
