#pragma once

#include <Arduino.h>

#ifndef SG98_RELEASE_VERSION
#define SG98_RELEASE_VERSION "0.0.0"
#endif

#ifndef SG98_FIRMWARE_ROLE
#define SG98_FIRMWARE_ROLE "unknown"
#endif

#ifndef SG98_BUILD_ENV
#define SG98_BUILD_ENV "unknown"
#endif

#ifndef SG98_GIT_SHA
#define SG98_GIT_SHA "nogit"
#endif

#ifndef SG98_GIT_DIRTY
#define SG98_GIT_DIRTY 0
#endif

#ifndef SG98_BUILD_DATE
#define SG98_BUILD_DATE __DATE__
#endif

#ifndef SG98_BUILD_TIME
#define SG98_BUILD_TIME __TIME__
#endif

namespace sg98_version {

inline const char* release() {
    return SG98_RELEASE_VERSION;
}

inline const char* role() {
    return SG98_FIRMWARE_ROLE;
}

inline const char* buildEnv() {
    return SG98_BUILD_ENV;
}

inline const char* gitSha() {
    return SG98_GIT_SHA;
}

inline bool dirty() {
    return SG98_GIT_DIRTY != 0;
}

inline String gitLabel() {
    String out = SG98_GIT_SHA;
    if(dirty()) {
        out += "-dirty";
    }
    return out;
}

inline String shortVersion() {
    return String("v") + SG98_RELEASE_VERSION;
}

inline String aboutTitle() {
    return String("SG98 ") + SG98_FIRMWARE_ROLE;
}

inline String aboutVersionLine() {
    return shortVersion() + " " + gitLabel();
}

inline String aboutBuildLine() {
    return String(SG98_BUILD_ENV) + " " + SG98_BUILD_DATE;
}

inline String buildId() {
    return String(SG98_FIRMWARE_ROLE) + " " + SG98_RELEASE_VERSION + " " +
           gitLabel() + " " + SG98_BUILD_ENV + " " + SG98_BUILD_DATE + " " +
           SG98_BUILD_TIME;
}

}  // namespace sg98_version
