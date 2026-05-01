#pragma once

#include <Arduino.h>

#include "sg98_ota_download.h"
#include "sg98_ota_source.h"
#include "sg98_version.h"

namespace sg98_ota_update {

constexpr uint32_t kOnlineCheckIntervalMs = 60UL * 60UL * 1000UL;

struct State {
    bool checked = false;
    bool updateAvailable = false;
    bool lastAttemptOk = false;
    uint32_t lastAttemptAt = 0;
    uint32_t lastSuccessAt = 0;
    String latestVersion;
    String latestGitSha;
    bool latestGitDirty = false;
    bool latestGitDirtyPresent = false;
    String latestGeneratedAtUtc;
    String latestImageSha256;
    uint32_t latestImageSize = 0;
    String message;
};

inline bool isDigit(char ch) {
    return ch >= '0' && ch <= '9';
}

inline int compareDottedVersion(const String& lhs, const String& rhs) {
    size_t li = 0;
    size_t ri = 0;
    while(li < lhs.length() || ri < rhs.length()) {
        while(li < lhs.length() && !isDigit(lhs[li])) {
            ++li;
        }
        while(ri < rhs.length() && !isDigit(rhs[ri])) {
            ++ri;
        }
        if(li >= lhs.length() || ri >= rhs.length()) {
            break;
        }

        uint32_t lv = 0;
        uint32_t rv = 0;
        while(li < lhs.length() && isDigit(lhs[li])) {
            lv = lv * 10UL + static_cast<uint32_t>(lhs[li] - '0');
            ++li;
        }
        while(ri < rhs.length() && isDigit(rhs[ri])) {
            rv = rv * 10UL + static_cast<uint32_t>(rhs[ri] - '0');
            ++ri;
        }
        if(lv < rv) return -1;
        if(lv > rv) return 1;
    }

    while(li < lhs.length() && !isDigit(lhs[li])) {
        ++li;
    }
    while(ri < rhs.length() && !isDigit(rhs[ri])) {
        ++ri;
    }
    if(li < lhs.length()) return 1;
    if(ri < rhs.length()) return -1;
    return 0;
}

inline bool manifestLooksNewer(const sg98_ota_download::ManifestSpec& spec) {
    const int versionCmp = compareDottedVersion(spec.version, sg98_version::release());
    if(versionCmp > 0) {
        return true;
    }
    if(versionCmp < 0) {
        return false;
    }

    if(spec.gitSha.length() > 0 && spec.gitSha != sg98_version::gitSha()) {
        return true;
    }
    if(spec.gitDirtyPresent && spec.gitDirty != sg98_version::dirty()) {
        return true;
    }
    return false;
}

inline bool shouldCheck(const State& state, uint32_t now) {
    return state.lastAttemptAt == 0 ||
           static_cast<uint32_t>(now - state.lastAttemptAt) >= kOnlineCheckIntervalMs;
}

inline bool checkLatestManifest(State* state, uint32_t now) {
    if(state == nullptr) {
        return false;
    }

    state->lastAttemptAt = now;
    sg98_ota_download::ManifestSpec spec;
    sg98_ota_download::Result result;
    const String manifestUrl = sg98_ota_source::latestManifestUrl();
    if(!sg98_ota_download::fetchManifestSpec(manifestUrl, &spec, &result)) {
        state->lastAttemptOk = false;
        state->message = result.message.length() > 0 ? result.message : String("check failed");
        return false;
    }

    state->checked = true;
    state->lastAttemptOk = true;
    state->lastSuccessAt = now;
    state->latestVersion = spec.version;
    state->latestGitSha = spec.gitSha;
    state->latestGitDirty = spec.gitDirty;
    state->latestGitDirtyPresent = spec.gitDirtyPresent;
    state->latestGeneratedAtUtc = spec.generatedAtUtc;
    state->latestImageSha256 = spec.sha256;
    state->latestImageSize = spec.size;
    state->updateAvailable = manifestLooksNewer(spec);
    state->message = state->updateAvailable ? "update available" : "current";
    return true;
}

inline String compactGit(const String& gitSha) {
    if(gitSha.length() == 0) {
        return String();
    }
    return gitSha.substring(0, min<size_t>(8, gitSha.length()));
}

inline String latestLabel(const State& state) {
    String out = "v";
    out += state.latestVersion.length() > 0 ? state.latestVersion : String("?");
    const String git = compactGit(state.latestGitSha);
    if(git.length() > 0) {
        out += " ";
        out += git;
    }
    return out;
}

inline String statusLine(const State& state) {
    if(state.checked) {
        return state.updateAvailable ? String("NEW ") + latestLabel(state) : String("CURRENT");
    }
    if(state.lastAttemptAt != 0) {
        return String("CHECK FAIL");
    }
    return String("SYNC WAIT");
}

inline String detailLine(const State& state) {
    if(state.checked) {
        return latestLabel(state);
    }
    if(state.message.length() > 0) {
        return state.message;
    }
    return String("Waiting for internet sync");
}

}  // namespace sg98_ota_update
