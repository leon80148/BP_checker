#ifndef BP_BUILD_INFO_H
#define BP_BUILD_INFO_H

#define BP_FIRMWARE_VERSION "0.1.0-dev"

#define BP_STRINGIFY_INNER(value) #value
#define BP_STRINGIFY(value) BP_STRINGIFY_INNER(value)

#ifndef BP_BUILD_SHA_TOKEN
#define BP_BUILD_SHA_TOKEN development
#endif

#define BP_BUILD_SHA BP_STRINGIFY(BP_BUILD_SHA_TOKEN)

#endif
