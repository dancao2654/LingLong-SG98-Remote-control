#include "error_history.h"

#include <Preferences.h>

namespace {
constexpr uint16_t ERROR_HISTORY_CAPACITY = 1024;
constexpr uint16_t ERROR_HISTORY_PERSIST_CAPACITY = 64;
constexpr uint16_t ERROR_HISTORY_PERSIST_VERSION = 2;

struct __attribute__((packed)) PackedErrorHistoryEntry {
    uint32_t sequence;
    uint32_t uptime_ms;
    uint32_t value;
    uint16_t code;
    uint8_t source;
    uint8_t level;
    uint8_t state;
};

ErrorHistoryEntry errorHistoryEntries[ERROR_HISTORY_CAPACITY];
uint16_t errorHistoryHead = 0;
uint16_t errorHistoryCountValue = 0;
uint32_t errorHistorySequence = 0;
uint16_t persistentHistoryHead = 0;
uint16_t persistentHistoryCountValue = 0;
bool errorHistoryInitialized = false;
bool persistentHistoryAvailable = false;
Preferences errorHistoryPreferences;

void makeEntryKey(uint16_t slot, char* buffer, size_t bufferSize) {
    snprintf(buffer, bufferSize, "e%03u", static_cast<unsigned>(slot));
}

void resetRamHistory() {
    errorHistoryHead = 0;
    errorHistoryCountValue = 0;
    errorHistorySequence = 0;
}

PackedErrorHistoryEntry packEntry(const ErrorHistoryEntry& entry) {
    PackedErrorHistoryEntry packed = {};
    packed.sequence = entry.sequence;
    packed.uptime_ms = entry.uptime_ms;
    packed.value = entry.value;
    packed.code = entry.code;
    packed.source = entry.source;
    packed.level = entry.level;
    packed.state = entry.state;
    return packed;
}

ErrorHistoryEntry unpackEntry(const PackedErrorHistoryEntry& packed) {
    ErrorHistoryEntry entry = {};
    entry.sequence = packed.sequence;
    entry.uptime_ms = packed.uptime_ms;
    entry.value = packed.value;
    entry.code = packed.code;
    entry.source = packed.source;
    entry.level = packed.level;
    entry.state = packed.state;
    return entry;
}

bool shouldPersistEntry(const ErrorHistoryEntry& entry) {
    if(entry.level >= ERROR_HISTORY_LEVEL_WARN) {
        return true;
    }

    switch(entry.code) {
        case ERROR_HISTORY_CODE_MOTOR_PROBE:
        case ERROR_HISTORY_CODE_MOTOR_RPM_TEST:
        case ERROR_HISTORY_CODE_MOTOR_JMC_DIAG:
        case ERROR_HISTORY_CODE_MOTOR_JMC_BAUD:
        case ERROR_HISTORY_CODE_MOTOR_JMC_BAUD_TRIAL:
            return true;
        default:
            return false;
    }
}

void resetPersistentHistory() {
    if(!persistentHistoryAvailable) {
        return;
    }

    errorHistoryPreferences.clear();
    errorHistoryPreferences.putUShort("fmt", ERROR_HISTORY_PERSIST_VERSION);
    errorHistoryPreferences.putUShort("head", 0);
    errorHistoryPreferences.putUShort("count", 0);
    errorHistoryPreferences.putUInt("seq", 0);
    persistentHistoryHead = 0;
    persistentHistoryCountValue = 0;
}

void loadPersistentHistory() {
    if(!persistentHistoryAvailable) {
        return;
    }

    const uint16_t persistedVersion = errorHistoryPreferences.getUShort("fmt", 0);
    if(persistedVersion != ERROR_HISTORY_PERSIST_VERSION) {
        resetPersistentHistory();
        return;
    }

    persistentHistoryHead = errorHistoryPreferences.getUShort("head", 0);
    persistentHistoryCountValue = errorHistoryPreferences.getUShort("count", 0);
    errorHistorySequence = errorHistoryPreferences.getUInt("seq", 0);

    if(persistentHistoryHead >= ERROR_HISTORY_PERSIST_CAPACITY) {
        persistentHistoryHead = 0;
    }
    if(persistentHistoryCountValue > ERROR_HISTORY_PERSIST_CAPACITY) {
        persistentHistoryCountValue = ERROR_HISTORY_PERSIST_CAPACITY;
    }

    if(persistentHistoryCountValue == 0) {
        return;
    }

    const uint16_t oldestSlot =
        (persistentHistoryHead + ERROR_HISTORY_PERSIST_CAPACITY - persistentHistoryCountValue) %
        ERROR_HISTORY_PERSIST_CAPACITY;

    for(uint16_t i = 0; i < persistentHistoryCountValue && i < ERROR_HISTORY_CAPACITY; ++i) {
        char key[8];
        makeEntryKey((oldestSlot + i) % ERROR_HISTORY_PERSIST_CAPACITY, key, sizeof(key));
        PackedErrorHistoryEntry packed = {};
        const size_t read = errorHistoryPreferences.getBytes(key, &packed, sizeof(packed));
        if(read != sizeof(packed)) {
            continue;
        }

        errorHistoryEntries[errorHistoryHead] = unpackEntry(packed);
        errorHistoryHead = (errorHistoryHead + 1U) % ERROR_HISTORY_CAPACITY;
        if(errorHistoryCountValue < ERROR_HISTORY_CAPACITY) {
            errorHistoryCountValue++;
        }
    }
}

void persistEntry(const ErrorHistoryEntry& entry) {
    if(!persistentHistoryAvailable) {
        return;
    }

    if(!shouldPersistEntry(entry)) {
        return;
    }

    char key[8];
    makeEntryKey(persistentHistoryHead, key, sizeof(key));
    const PackedErrorHistoryEntry packed = packEntry(entry);
    bool wrote = errorHistoryPreferences.putBytes(key, &packed, sizeof(packed)) == sizeof(packed);
    if(!wrote) {
        resetPersistentHistory();
        makeEntryKey(persistentHistoryHead, key, sizeof(key));
        wrote = errorHistoryPreferences.putBytes(key, &packed, sizeof(packed)) == sizeof(packed);
        if(!wrote) {
            return;
        }
    }

    persistentHistoryHead = (persistentHistoryHead + 1U) % ERROR_HISTORY_PERSIST_CAPACITY;
    if(persistentHistoryCountValue < ERROR_HISTORY_PERSIST_CAPACITY) {
        persistentHistoryCountValue++;
    }

    const bool wroteHead = errorHistoryPreferences.putUShort("head", persistentHistoryHead) == sizeof(uint16_t);
    const bool wroteCount = errorHistoryPreferences.putUShort("count", persistentHistoryCountValue) == sizeof(uint16_t);
    const bool wroteSeq = errorHistoryPreferences.putUInt("seq", errorHistorySequence) == sizeof(uint32_t);
    if(!wroteHead || !wroteCount || !wroteSeq) {
        resetPersistentHistory();
        makeEntryKey(persistentHistoryHead, key, sizeof(key));
        if(errorHistoryPreferences.putBytes(key, &packed, sizeof(packed)) != sizeof(packed)) {
            return;
        }
        persistentHistoryHead = (persistentHistoryHead + 1U) % ERROR_HISTORY_PERSIST_CAPACITY;
        persistentHistoryCountValue = 1;
        errorHistoryPreferences.putUShort("head", persistentHistoryHead);
        errorHistoryPreferences.putUShort("count", persistentHistoryCountValue);
        errorHistoryPreferences.putUInt("seq", errorHistorySequence);
    }
}
}

void error_history_init() {
    if(errorHistoryInitialized) {
        return;
    }

    resetRamHistory();
    persistentHistoryAvailable = errorHistoryPreferences.begin("elog", false);
    loadPersistentHistory();
    errorHistoryInitialized = true;
}

void error_history_log(uint8_t source,
                       uint8_t level,
                       uint16_t code,
                       uint32_t value,
                       uint8_t state) {
    if(!errorHistoryInitialized) {
        error_history_init();
    }

    ErrorHistoryEntry& entry = errorHistoryEntries[errorHistoryHead];
    entry.sequence = ++errorHistorySequence;
    entry.uptime_ms = millis();
    entry.value = value;
    entry.code = code;
    entry.source = source;
    entry.level = level;
    entry.state = state;

    errorHistoryHead = (errorHistoryHead + 1U) % ERROR_HISTORY_CAPACITY;
    if(errorHistoryCountValue < ERROR_HISTORY_CAPACITY) {
        errorHistoryCountValue++;
    }

    persistEntry(entry);
}

uint16_t error_history_count() {
    return errorHistoryCountValue;
}

bool error_history_get(uint16_t index_from_oldest, ErrorHistoryEntry* entry) {
    if(entry == nullptr || index_from_oldest >= errorHistoryCountValue) {
        return false;
    }

    const uint16_t oldestIndex =
        (errorHistoryHead + ERROR_HISTORY_CAPACITY - errorHistoryCountValue) % ERROR_HISTORY_CAPACITY;
    const uint16_t index = (oldestIndex + index_from_oldest) % ERROR_HISTORY_CAPACITY;
    *entry = errorHistoryEntries[index];
    return true;
}
