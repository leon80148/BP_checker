#ifndef BP_RELEASE_TRUST_ANCHOR_H
#define BP_RELEASE_TRUST_ANCHOR_H

// Release builds inject a DER SubjectPublicKeyInfo value as lowercase hex.
// The corresponding private key must remain outside source, firmware, and CI
// logs. An ordinary development build intentionally has no trusted signer and
// therefore rejects every update.
#ifndef BP_RELEASE_PUBLIC_KEY_DER_HEX
#define BP_RELEASE_PUBLIC_KEY_DER_HEX ""
#endif

inline constexpr char kReleasePublicKeyDerHex[] =
  BP_RELEASE_PUBLIC_KEY_DER_HEX;

#endif
