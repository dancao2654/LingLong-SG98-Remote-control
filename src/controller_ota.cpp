#include "controller_ota.h"

#include <Preferences.h>
#include <cstring>
#include <esp_ota_ops.h>

#include "controller_board.h"

namespace {

constexpr const char* kPrefsNamespace = "sg98_ota";
constexpr const char* kReadyKey = "ready";
constexpr const char* kSlotKey = "slot";
constexpr const char* kOffsetKey = "offset";
constexpr const char* kVersionKey = "ver";
constexpr const char* kElfShaKey = "elfsha";
constexpr const char* kImageShaKey = "imgsha";
constexpr uint32_t kReadyValidateMs = 1000;

struct OtaReadyMarker {
    bool loaded = false;
    bool ready = false;
    String slotLabel;
    uint32_t slotOffset = 0;
    String version;
    String elfSha256;
    String imageSha256;
};

OtaReadyMarker g_marker;
uint32_t g_lastValidateAt = 0;
bool g_cachedReady = false;
String g_cachedDetail;

String appDescString(const char* value, size_t maxLen) {
    if(value == nullptr || maxLen == 0) {
        return String();
    }
    String out;
    out.reserve(maxLen);
    for(size_t idx = 0; idx < maxLen && value[idx] != '\0'; ++idx) {
        out += value[idx];
    }
    return out;
}

String appElfSha256(const esp_app_desc_t& desc) {
    return sg98_ota_download::sha256ToHex(desc.app_elf_sha256);
}

void invalidateReadyCache() {
    g_lastValidateAt = 0;
    g_cachedReady = false;
    g_cachedDetail = "";
}

void loadMarkerIfNeeded() {
    if(g_marker.loaded) {
        return;
    }

    g_marker = {};
    g_marker.loaded = true;
    Preferences prefs;
    if(!prefs.begin(kPrefsNamespace, true)) {
        return;
    }
    g_marker.ready = prefs.getBool(kReadyKey, false);
    g_marker.slotLabel = prefs.getString(kSlotKey, "");
    g_marker.slotOffset = prefs.getUInt(kOffsetKey, 0);
    g_marker.version = prefs.getString(kVersionKey, "");
    g_marker.elfSha256 = prefs.getString(kElfShaKey, "");
    g_marker.imageSha256 = prefs.getString(kImageShaKey, "");
    prefs.end();
}

void saveMarker() {
    Preferences prefs;
    if(!prefs.begin(kPrefsNamespace, false)) {
        return;
    }
    prefs.putBool(kReadyKey, g_marker.ready);
    prefs.putString(kSlotKey, g_marker.slotLabel);
    prefs.putUInt(kOffsetKey, g_marker.slotOffset);
    prefs.putString(kVersionKey, g_marker.version);
    prefs.putString(kElfShaKey, g_marker.elfSha256);
    prefs.putString(kImageShaKey, g_marker.imageSha256);
    prefs.end();
    invalidateReadyCache();
}

bool partitionLabelEquals(const esp_partition_t* partition, const String& label) {
    return partition != nullptr && label.length() > 0 &&
           strncmp(partition->label, label.c_str(), sizeof(partition->label)) == 0;
}

bool readPartitionDescription(const esp_partition_t* partition, esp_app_desc_t* desc) {
    if(partition == nullptr || desc == nullptr) {
        return false;
    }
    memset(desc, 0, sizeof(*desc));
    return esp_ota_get_partition_description(partition, desc) == ESP_OK &&
           desc->magic_word == ESP_APP_DESC_MAGIC_WORD;
}

bool markerMatchesNextSlot(const esp_partition_t** outPartition = nullptr, esp_app_desc_t* outDesc = nullptr) {
    loadMarkerIfNeeded();
    if(!g_marker.ready || g_marker.slotLabel.length() == 0) {
        return false;
    }

    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    if(!partitionLabelEquals(partition, g_marker.slotLabel)) {
        return false;
    }
    if(g_marker.slotOffset != 0 && partition->address != g_marker.slotOffset) {
        return false;
    }

    esp_app_desc_t desc = {};
    if(!readPartitionDescription(partition, &desc)) {
        return false;
    }
    const String elfSha = appElfSha256(desc);
    if(g_marker.elfSha256.length() > 0 && elfSha != g_marker.elfSha256) {
        return false;
    }

    if(outPartition != nullptr) {
        *outPartition = partition;
    }
    if(outDesc != nullptr) {
        *outDesc = desc;
    }
    return true;
}

void clearMarker() {
    g_marker.loaded = true;
    g_marker.ready = false;
    g_marker.slotLabel = "";
    g_marker.slotOffset = 0;
    g_marker.version = "";
    g_marker.elfSha256 = "";
    g_marker.imageSha256 = "";

    Preferences prefs;
    if(prefs.begin(kPrefsNamespace, false)) {
        prefs.clear();
        prefs.end();
    }
    invalidateReadyCache();
}

void finalizeBootedReadySlotIfNeeded() {
    loadMarkerIfNeeded();
    if(!g_marker.ready || g_marker.slotLabel.length() == 0) {
        return;
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    if(!partitionLabelEquals(running, g_marker.slotLabel)) {
        const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
        if(!partitionLabelEquals(next, g_marker.slotLabel)) {
            Serial.printf("[ota] clearing stale ready marker slot=%s\n", g_marker.slotLabel.c_str());
            clearMarker();
        }
        return;
    }

    const esp_err_t markErr = esp_ota_mark_app_valid_cancel_rollback();
    String invalidateMessage;
    const bool invalidated = controller_ota_invalidate_inactive_slot(&invalidateMessage);
    Serial.printf("[ota] booted ready slot=%s version=%s mark_valid=%d invalidate_old=%d %s\n",
                  g_marker.slotLabel.c_str(),
                  g_marker.version.c_str(),
                  static_cast<int>(markErr),
                  invalidated ? 1 : 0,
                  invalidateMessage.c_str());
    clearMarker();
}

}  // namespace

void controller_ota_init() {
    loadMarkerIfNeeded();
    finalizeBootedReadySlotIfNeeded();
}

bool controller_ota_note_downloaded(const sg98_ota_download::Result& result) {
    if(!result.ok) {
        return false;
    }

    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    if(partition == nullptr) {
        return false;
    }

    esp_app_desc_t desc = {};
    if(!readPartitionDescription(partition, &desc)) {
        Serial.println("[ota] downloaded image missing app description; ready marker not set");
        return false;
    }

    g_marker.loaded = true;
    g_marker.ready = true;
    g_marker.slotLabel = partition->label;
    g_marker.slotOffset = partition->address;
    g_marker.version = result.version.length() > 0
                           ? result.version
                           : appDescString(desc.version, sizeof(desc.version));
    g_marker.elfSha256 = appElfSha256(desc);
    g_marker.imageSha256 = result.sha256;
    saveMarker();

    const String shortElf = g_marker.elfSha256.substring(0, 12);
    Serial.printf("[ota] ready slot=%s offset=0x%lx version=%s elf=%s\n",
                  g_marker.slotLabel.c_str(),
                  static_cast<unsigned long>(g_marker.slotOffset),
                  g_marker.version.c_str(),
                  shortElf.c_str());
    return true;
}

bool controller_ota_ready_for_remote() {
    if(!controller_board_requires_remote_recovery()) {
        return false;
    }

    const uint32_t now = millis();
    if(g_lastValidateAt != 0 && static_cast<uint32_t>(now - g_lastValidateAt) < kReadyValidateMs) {
        return g_cachedReady;
    }

    const esp_partition_t* partition = nullptr;
    esp_app_desc_t desc = {};
    g_cachedReady = markerMatchesNextSlot(&partition, &desc);
    g_lastValidateAt = now;
    if(g_cachedReady && partition != nullptr) {
        const String version = g_marker.version.length() > 0
                                   ? g_marker.version
                                   : appDescString(desc.version, sizeof(desc.version));
        g_cachedDetail = String(partition->label) + " " + version;
    } else {
        g_cachedDetail = "";
    }
    return g_cachedReady;
}

bool controller_ota_ready_matches(const sg98_ota_download::ManifestSpec& spec) {
    if(!controller_ota_ready_for_remote()) {
        return false;
    }

    loadMarkerIfNeeded();
    if(spec.sha256.length() > 0 &&
       g_marker.imageSha256.length() > 0 &&
       !g_marker.imageSha256.equalsIgnoreCase(spec.sha256)) {
        return false;
    }
    if(spec.version.length() > 0 &&
       g_marker.version.length() > 0 &&
       g_marker.version != spec.version) {
        return false;
    }
    return true;
}

String controller_ota_ready_detail() {
    (void)controller_ota_ready_for_remote();
    return g_cachedDetail;
}

void controller_ota_clear_ready() {
    clearMarker();
}

bool controller_ota_mark_ready_bootable(String* message) {
    const esp_partition_t* partition = nullptr;
    esp_app_desc_t desc = {};
    if(!markerMatchesNextSlot(&partition, &desc) || partition == nullptr) {
        if(message != nullptr) {
            *message = "no ota ready";
        }
        return false;
    }

    const esp_err_t err = esp_ota_set_boot_partition(partition);
    if(err != ESP_OK) {
        if(message != nullptr) {
            *message = String("set boot failed err=") + String(static_cast<int>(err));
        }
        return false;
    }

    if(message != nullptr) {
        *message = String("boot=") + partition->label + " " +
                   (g_marker.version.length() > 0 ? g_marker.version
                                                  : appDescString(desc.version, sizeof(desc.version))) +
                   " reboot required";
    }
    invalidateReadyCache();
    return true;
}

bool controller_ota_invalidate_inactive_slot(String* message) {
    const esp_err_t err = esp_ota_invalidate_inactive_ota_data_slot();
    if(err != ESP_OK) {
        if(message != nullptr) {
            *message = String("invalidate inactive failed err=") + String(static_cast<int>(err));
        }
        return false;
    }
    if(message != nullptr) {
        *message = "inactive ota data invalidated";
    }
    return true;
}
