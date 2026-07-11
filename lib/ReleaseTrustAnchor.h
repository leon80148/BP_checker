#ifndef BP_RELEASE_TRUST_ANCHOR_H
#define BP_RELEASE_TRUST_ANCHOR_H

// Release builds inject a DER SubjectPublicKeyInfo value as one lowercase-hex
// preprocessor token; stringification avoids fragile shell/compiler quote layers.
// The corresponding private key must remain outside source, firmware, and CI
// logs. An ordinary development build intentionally has no trusted signer and
// therefore rejects every update.
#ifndef BP_RELEASE_PUBLIC_KEY_DER_HEX
#define BP_RELEASE_PUBLIC_KEY_DER_HEX unconfigured
#endif

#define BP_RELEASE_STRINGIFY_INNER(value) #value
#define BP_RELEASE_STRINGIFY(value) BP_RELEASE_STRINGIFY_INNER(value)

inline constexpr char kReleasePublicKeyDerHex[] =
  BP_RELEASE_STRINGIFY(BP_RELEASE_PUBLIC_KEY_DER_HEX);

#endif
