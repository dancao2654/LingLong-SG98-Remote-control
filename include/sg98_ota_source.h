#pragma once

#include <Arduino.h>

#include "sg98_version.h"

namespace sg98_ota_source {

constexpr const char* kPublicOtaBaseUrl =
    "https://raw.githubusercontent.com/dancao2654/SG98-firmware-releases/main/ota/";

inline String latestManifestUrl() {
    return String(kPublicOtaBaseUrl) + sg98_version::buildEnv() + "/sg98-ota-manifest.json";
}

inline bool isLatestToken(const String& token) {
    return token.length() == 0 || token == "latest" || token == "public" || token == "default";
}

inline String resolveManifestUrl(String token) {
    token.trim();
    String lowered = token;
    lowered.toLowerCase();
    if(isLatestToken(lowered)) {
        return latestManifestUrl();
    }
    return token;
}

}  // namespace sg98_ota_source
