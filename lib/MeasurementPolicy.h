#ifndef BP_MEASUREMENT_POLICY_H
#define BP_MEASUREMENT_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "BPProtocol.h"

// Presentation-only review policy. It provides deterministic operator cues;
// it does not diagnose a condition or replace clinician review.
struct MeasurementPolicyConfig {
  int reviewSystolic = 130;
  int reviewDiastolic = 80;
  int reviewPulseLow = 60;
  int reviewPulseHigh = 100;
  int urgentSystolic = 180;
  int urgentDiastolic = 120;
  uint32_t staleAfterMs = 300000U;
};

enum class MeasurementReviewState : uint8_t {
  INVALID = 0,
  WITHIN_REFERENCE,
  REVIEW,
  URGENT,
};

enum class MeasurementFreshnessState : uint8_t {
  INVALID = 0,
  CURRENT,
  STALE,
  HISTORICAL,
  DISCONNECTED,
};

struct MeasurementFreshnessInput {
  bool hasRecord = false;
  bool valid = false;
  bool receivedThisBoot = false;
  bool transportConnected = false;
  uint32_t nowMs = 0;
  uint32_t lastSuccessfulReceiveMs = 0;
  uint32_t staleAfterMs = MeasurementPolicyConfig{}.staleAfterMs;
};

inline bool formatOpaqueSequence(uint64_t value, char* output,
                                 size_t capacity) {
  if (output == nullptr || capacity == 0) return false;
  const int length = snprintf(output, capacity, "%llu",
    static_cast<unsigned long long>(value));
  if (length <= 0 || static_cast<size_t>(length) >= capacity) {
    output[0] = '\0';
    return false;
  }
  return true;
}

inline bool validMeasurementPolicy(const MeasurementPolicyConfig& policy) {
  return policy.reviewSystolic >= 60 &&
         policy.reviewSystolic < policy.urgentSystolic &&
         policy.urgentSystolic <= 260 &&
         policy.reviewDiastolic >= 30 &&
         policy.reviewDiastolic < policy.urgentDiastolic &&
         policy.urgentDiastolic <= 215 &&
         policy.reviewPulseLow >= 40 &&
         policy.reviewPulseLow < policy.reviewPulseHigh &&
         policy.reviewPulseHigh <= 180 &&
         policy.staleAfterMs != 0;
}

inline bool validMeasurementForReview(const BPData& value) {
  return value.valid &&
         value.systolic >= 60 && value.systolic <= 260 &&
         value.diastolic >= 30 && value.diastolic <= 215 &&
         value.pulse >= 40 && value.pulse <= 180;
}

inline MeasurementReviewState classifyMeasurement(
    const BPData& value,
    const MeasurementPolicyConfig& policy = MeasurementPolicyConfig{}) {
  if (!validMeasurementPolicy(policy) ||
      !validMeasurementForReview(value)) {
    return MeasurementReviewState::INVALID;
  }
  if (value.systolic > policy.urgentSystolic ||
      value.diastolic > policy.urgentDiastolic) {
    return MeasurementReviewState::URGENT;
  }
  if (value.systolic >= policy.reviewSystolic ||
      value.diastolic >= policy.reviewDiastolic ||
      value.pulse < policy.reviewPulseLow ||
      value.pulse > policy.reviewPulseHigh) {
    return MeasurementReviewState::REVIEW;
  }
  return MeasurementReviewState::WITHIN_REFERENCE;
}

inline const char* measurementReviewCode(MeasurementReviewState state) {
  switch (state) {
    case MeasurementReviewState::WITHIN_REFERENCE: return "within_reference";
    case MeasurementReviewState::REVIEW:           return "review";
    case MeasurementReviewState::URGENT:           return "urgent";
    case MeasurementReviewState::INVALID:
    default:                                       return "invalid";
  }
}

inline const char* measurementReviewLabel(MeasurementReviewState state) {
  switch (state) {
    case MeasurementReviewState::WITHIN_REFERENCE:
      return "未達設定複核門檻";
    case MeasurementReviewState::REVIEW:
      return "達到設定複核門檻";
    case MeasurementReviewState::URGENT:
      return "達到緊急處置提示門檻";
    case MeasurementReviewState::INVALID:
    default:
      return "量測值不可用";
  }
}

inline MeasurementFreshnessState measurementFreshness(
    const MeasurementFreshnessInput& input) {
  if (!input.hasRecord || !input.valid) {
    return MeasurementFreshnessState::INVALID;
  }
  // A reboot removes receipt-time provenance. Durable records remain useful
  // history but can never masquerade as a live measurement.
  if (!input.receivedThisBoot) {
    return MeasurementFreshnessState::HISTORICAL;
  }
  if (!input.transportConnected) {
    return MeasurementFreshnessState::DISCONNECTED;
  }
  const uint32_t age = input.nowMs - input.lastSuccessfulReceiveMs;
  if (input.staleAfterMs == 0 || age >= input.staleAfterMs) {
    return MeasurementFreshnessState::STALE;
  }
  return MeasurementFreshnessState::CURRENT;
}

inline const char* measurementFreshnessCode(
    MeasurementFreshnessState state) {
  switch (state) {
    case MeasurementFreshnessState::CURRENT:      return "current";
    case MeasurementFreshnessState::STALE:        return "stale";
    case MeasurementFreshnessState::HISTORICAL:   return "historical";
    case MeasurementFreshnessState::DISCONNECTED: return "disconnected";
    case MeasurementFreshnessState::INVALID:
    default:                                      return "invalid";
  }
}

inline const char* measurementFreshnessLabel(
    MeasurementFreshnessState state) {
  switch (state) {
    case MeasurementFreshnessState::CURRENT:
      return "本次開機的新量測";
    case MeasurementFreshnessState::STALE:
      return "量測已逾時，請重新量測";
    case MeasurementFreshnessState::HISTORICAL:
      return "開機前歷史量測";
    case MeasurementFreshnessState::DISCONNECTED:
      return "資料通道已中斷，顯示最後量測";
    case MeasurementFreshnessState::INVALID:
    default:
      return "尚無可用量測";
  }
}

inline const char* timestampSourceCode(BPTimestampSource source) {
  switch (source) {
    case BPTimestampSource::DEVICE:          return "device";
    case BPTimestampSource::SYSTEM:          return "system";
    case BPTimestampSource::LEGACY_SYSTEM:   return "legacy_system";
    case BPTimestampSource::LEGACY_UNSYNCED: return "legacy_unsynced";
    case BPTimestampSource::UNSYNCED:
    default:                                 return "unsynced";
  }
}

inline const char* measurementQualityCode(BPMeasurementQuality quality) {
  return quality == BPMeasurementQuality::MOTION ? "movement" : "clean";
}

inline const char* measurementReferencePolicyName() {
  return "血壓參考：2025 AHA/ACC 成人高血壓指引；實際門檻與脈搏規則由診所設定";
}

inline const char* repeatedMeasurementGuidance() {
  return "請依診所流程靜坐後再量測一次，兩次量測至少間隔 1 分鐘；"
         "系統會分別保留結果，不會自動平均。";
}

inline const char* supportedMeasurementProtocol() {
  return "OMRON HBP-9030 USB function item 32 format 5";
}

#endif
