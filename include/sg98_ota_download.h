#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <cJSON.h>
#include <lwip/netdb.h>
#include <mbedtls/sha256.h>

#include "sg98_version.h"

namespace sg98_ota_download {

constexpr size_t kOtaDownloadBufferSize = 2048;
constexpr size_t kMaxManifestBytes = 16384;
constexpr uint8_t kMaxHttpsRedirects = 3;

struct Result {
    bool ok = false;
    bool fromManifest = false;
    bool verified = false;
    String message;
    uint32_t written = 0;
    uint32_t expected = 0;
    String slotLabel;
    String slotOffset;
    String version;
    String sha256;
    String imageUrl;
};

struct DownloadOptions {
    using ProgressCallback = void (*)(uint32_t written, uint32_t total, void* context);
    uint32_t expectedSize = 0;
    String expectedSha256;
    String version;
    ProgressCallback progress = nullptr;
    void* progressContext = nullptr;
};

struct HttpsUrl {
    String cleanUrl;
    String host;
    String path;
    uint16_t port = 443;
    IPAddress ipOverride;
};

struct ManifestSpec {
    String version;
    String gitSha;
    bool gitDirty = false;
    bool gitDirtyPresent = false;
    String generatedAtUtc;
    String role;
    String envName;
    String imageUrl;
    String sha256;
    uint32_t size = 0;
};

inline String offsetHex(uint32_t value) {
    char out[12];
    snprintf(out, sizeof(out), "0x%lx", static_cast<unsigned long>(value));
    return String(out);
}

inline void finish(Result* result, bool ok, const String& message) {
    if(result != nullptr) {
        result->ok = ok;
        result->message = message;
    }
}

inline bool isHexChar(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

inline String normalizeSha256(String value) {
    value.trim();
    value.toLowerCase();
    return value;
}

inline bool isSha256Hex(const String& value) {
    if(value.length() != 64) {
        return false;
    }
    for(size_t i = 0; i < value.length(); ++i) {
        if(!isHexChar(value[i])) {
            return false;
        }
    }
    return true;
}

inline String sha256ToHex(const uint8_t digest[32]) {
    static constexpr char kHex[] = "0123456789abcdef";
    String out;
    out.reserve(64);
    for(uint8_t i = 0; i < 32; ++i) {
        out += kHex[(digest[i] >> 4) & 0x0F];
        out += kHex[digest[i] & 0x0F];
    }
    return out;
}

inline bool parseHttpsUrl(const String& rawUrl, HttpsUrl* parsed) {
    if(parsed == nullptr) {
        return false;
    }
    *parsed = HttpsUrl();
    String url = rawUrl;
    url.trim();
    const int fragmentAt = url.indexOf('#');
    if(fragmentAt >= 0) {
        const String fragment = url.substring(fragmentAt + 1);
        url = url.substring(0, fragmentAt);
        if(fragment.startsWith("ip=")) {
            parsed->ipOverride.fromString(fragment.substring(3));
        }
    }
    if(!url.startsWith("https://")) {
        return false;
    }

    const int hostStart = 8;
    int pathStart = url.indexOf('/', hostStart);
    if(pathStart < 0) {
        pathStart = url.length();
    }
    String hostPort = url.substring(hostStart, pathStart);
    if(hostPort.length() == 0) {
        return false;
    }

    parsed->port = 443;
    const int colonAt = hostPort.lastIndexOf(':');
    if(colonAt > 0) {
        const int portValue = hostPort.substring(colonAt + 1).toInt();
        if(portValue <= 0 || portValue > 65535) {
            return false;
        }
        parsed->port = static_cast<uint16_t>(portValue);
        hostPort = hostPort.substring(0, colonAt);
    }

    parsed->cleanUrl = url;
    parsed->host = hostPort;
    parsed->path = pathStart < url.length() ? url.substring(pathStart) : String("/");
    return true;
}

inline String hostHeader(const HttpsUrl& parsed) {
    if(parsed.port == 443) {
        return parsed.host;
    }
    return parsed.host + ":" + String(parsed.port);
}

inline bool resolveHost(const String& host, IPAddress* out) {
    if(out == nullptr || host.length() == 0) {
        return false;
    }
    if(out->fromString(host)) {
        return true;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    const int err = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if(err != 0 || res == nullptr) {
        return false;
    }

    const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
    *out = IPAddress(addr->sin_addr.s_addr);
    freeaddrinfo(res);
    return true;
}

inline int parseStatusCode(const String& headers) {
    const int firstSpace = headers.indexOf(' ');
    if(firstSpace < 0 || firstSpace + 4 > headers.length()) {
        return -1;
    }
    return headers.substring(firstSpace + 1, firstSpace + 4).toInt();
}

inline String headerValue(const String& headers, const char* name) {
    if(name == nullptr || name[0] == '\0') {
        return String();
    }
    String lower = headers;
    lower.toLowerCase();
    String key = "\r\n";
    key += name;
    key += ":";
    key.toLowerCase();

    int keyAt = lower.indexOf(key);
    int valueAt = -1;
    if(keyAt >= 0) {
        valueAt = keyAt + key.length();
    } else {
        String firstKey = String(name) + ":";
        firstKey.toLowerCase();
        if(lower.startsWith(firstKey)) {
            valueAt = firstKey.length();
        }
    }
    if(valueAt < 0) {
        return String();
    }
    while(valueAt < headers.length() && (headers[valueAt] == ' ' || headers[valueAt] == '\t')) {
        ++valueAt;
    }
    const int endAt = headers.indexOf("\r\n", valueAt);
    String value = headers.substring(valueAt, endAt >= 0 ? endAt : headers.length());
    value.trim();
    return value;
}

inline int parseContentLength(const String& headers) {
    const String value = headerValue(headers, "content-length");
    if(value.length() == 0) {
        return -1;
    }
    return value.toInt();
}

inline bool isRedirectStatus(int statusCode) {
    return statusCode == 301 || statusCode == 302 || statusCode == 303 ||
           statusCode == 307 || statusCode == 308;
}

inline String stripFragment(String url) {
    const int fragmentAt = url.indexOf('#');
    if(fragmentAt >= 0) {
        url = url.substring(0, fragmentAt);
    }
    return url;
}

inline String resolveUrlReference(const String& baseUrl, const String& reference) {
    String ref = reference;
    ref.trim();
    if(ref.startsWith("https://") || ref.startsWith("http://")) {
        return ref;
    }

    HttpsUrl base;
    if(!parseHttpsUrl(baseUrl, &base)) {
        return ref;
    }
    const String origin = String("https://") + hostHeader(base);
    if(ref.startsWith("/")) {
        return origin + ref;
    }

    String path = base.path;
    const int queryAt = path.indexOf('?');
    if(queryAt >= 0) {
        path = path.substring(0, queryAt);
    }
    const int lastSlash = path.lastIndexOf('/');
    const String dir = lastSlash >= 0 ? path.substring(0, lastSlash + 1) : String("/");
    return origin + dir + ref;
}

inline bool readHttpsHeaders(WiFiClientSecure& client, String* headers, Result* result) {
    if(headers == nullptr) {
        return false;
    }
    headers->remove(0);
    headers->reserve(512);
    const uint32_t startedAt = millis();
    while(headers->indexOf("\r\n\r\n") < 0) {
        while(client.available() > 0) {
            *headers += static_cast<char>(client.read());
            if(headers->length() > 4096) {
                finish(result, false, "header too large");
                return false;
            }
            if(headers->indexOf("\r\n\r\n") >= 0) {
                return true;
            }
        }
        if(!client.connected() || static_cast<uint32_t>(millis() - startedAt) > 15000UL) {
            finish(result, false, "header timeout");
            return false;
        }
        delay(2);
        yield();
    }
    return true;
}

inline bool openHttpsGetOnce(const String& url,
                             const char* accept,
                             WiFiClientSecure* client,
                             String* headers,
                             String* effectiveUrl,
                             Result* result) {
    if(client == nullptr || headers == nullptr || effectiveUrl == nullptr) {
        return false;
    }
    HttpsUrl parsed;
    if(!parseHttpsUrl(url, &parsed)) {
        finish(result, false, "bad https url");
        return false;
    }

    IPAddress ip = parsed.ipOverride;
    if(static_cast<uint32_t>(ip) == 0 && !resolveHost(parsed.host, &ip)) {
        finish(result, false, "dns failed");
        return false;
    }

    client->stop();
    client->setInsecure();
    client->setTimeout(15000);
    if(!client->connect(ip, parsed.port, parsed.host.c_str(), nullptr, nullptr, nullptr)) {
        finish(result, false, "https connect failed");
        return false;
    }

    client->print(String("GET ") + parsed.path + " HTTP/1.1\r\n" +
                  "Host: " + hostHeader(parsed) + "\r\n" +
                  "User-Agent: SG98-OTA/1\r\n" +
                  "Accept: " + (accept != nullptr ? accept : "*/*") + "\r\n" +
                  "Connection: close\r\n\r\n");

    if(!readHttpsHeaders(*client, headers, result)) {
        client->stop();
        return false;
    }
    *effectiveUrl = parsed.cleanUrl;
    return true;
}

inline bool openHttpsGet(const String& url,
                         const char* accept,
                         WiFiClientSecure* client,
                         String* headers,
                         String* effectiveUrl,
                         Result* result) {
    String currentUrl = url;
    for(uint8_t redirect = 0; redirect <= kMaxHttpsRedirects; ++redirect) {
        if(!openHttpsGetOnce(currentUrl, accept, client, headers, effectiveUrl, result)) {
            return false;
        }
        const int statusCode = parseStatusCode(*headers);
        if(!isRedirectStatus(statusCode)) {
            return true;
        }
        const String location = headerValue(*headers, "location");
        client->stop();
        if(location.length() == 0) {
            finish(result, false, String("http ") + String(statusCode) + " missing location");
            return false;
        }
        currentUrl = resolveUrlReference(*effectiveUrl, location);
    }
    finish(result, false, "too many redirects");
    return false;
}

inline bool fetchHttpsString(const String& url,
                             const char* accept,
                             size_t maxBytes,
                             String* payload,
                             Result* result) {
    if(payload == nullptr) {
        return false;
    }
    payload->remove(0);

    WiFiClientSecure client;
    String headers;
    String effectiveUrl;
    if(!openHttpsGet(url, accept, &client, &headers, &effectiveUrl, result)) {
        return false;
    }

    const int statusCode = parseStatusCode(headers);
    if(statusCode != 200) {
        client.stop();
        finish(result, false, String("http ") + String(statusCode));
        return false;
    }
    const int contentLength = parseContentLength(headers);
    if(contentLength <= 0) {
        client.stop();
        finish(result, false, "missing content-length");
        return false;
    }
    if(static_cast<size_t>(contentLength) > maxBytes) {
        client.stop();
        finish(result, false, "manifest too large");
        return false;
    }

    payload->reserve(static_cast<unsigned int>(contentLength + 1));
    uint8_t buffer[256];
    uint32_t readTotal = 0;
    uint32_t lastProgressAt = millis();
    while((client.connected() || client.available() > 0) && readTotal < static_cast<uint32_t>(contentLength)) {
        const int available = client.available();
        if(available <= 0) {
            if(static_cast<uint32_t>(millis() - lastProgressAt) > 15000UL) {
                client.stop();
                finish(result, false, "manifest timeout");
                return false;
            }
            delay(2);
            yield();
            continue;
        }
        const uint32_t remaining = static_cast<uint32_t>(contentLength) - readTotal;
        const size_t toRead = static_cast<size_t>(min<uint32_t>(min<uint32_t>(available, sizeof(buffer)), remaining));
        const int bytesRead = client.readBytes(buffer, toRead);
        if(bytesRead <= 0) {
            client.stop();
            finish(result, false, "manifest read failed");
            return false;
        }
        for(int i = 0; i < bytesRead; ++i) {
            *payload += static_cast<char>(buffer[i]);
        }
        readTotal += static_cast<uint32_t>(bytesRead);
        lastProgressAt = millis();
        yield();
    }
    client.stop();

    if(readTotal != static_cast<uint32_t>(contentLength)) {
        finish(result, false, String("short manifest ") + String(readTotal) + "/" + String(contentLength));
        return false;
    }
    return true;
}

inline bool writeOtaBody(Stream& stream,
                         Client& client,
                         const esp_partition_t* partition,
                         int contentLength,
                         const DownloadOptions& options,
                         Result* result) {
    if(partition == nullptr) {
        finish(result, false, "no inactive ota slot");
        return false;
    }
    if(contentLength <= 0) {
        finish(result, false, "missing content-length");
        return false;
    }
    if(options.expectedSize > 0 && static_cast<uint32_t>(contentLength) != options.expectedSize) {
        finish(result,
               false,
               String("size mismatch ") + String(contentLength) + "/" + String(options.expectedSize));
        return false;
    }
    if(static_cast<uint32_t>(contentLength) > partition->size) {
        const String message = String("image too large ") + String(contentLength) + "/" + String(partition->size);
        finish(result, false, message);
        return false;
    }
    if(result != nullptr) {
        result->expected = static_cast<uint32_t>(contentLength);
        result->version = options.version;
    }

    const String expectedSha = normalizeSha256(options.expectedSha256);
    const bool verifySha = expectedSha.length() > 0;
    if(verifySha && !isSha256Hex(expectedSha)) {
        finish(result, false, "bad manifest sha256");
        return false;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(partition, contentLength, &handle);
    if(err != ESP_OK) {
        finish(result, false, String("ota begin err=") + String(static_cast<int>(err)));
        return false;
    }

    uint8_t* buffer = static_cast<uint8_t*>(malloc(kOtaDownloadBufferSize));
    if(buffer == nullptr) {
        (void)esp_ota_abort(handle);
        finish(result, false, "ota buffer alloc failed");
        return false;
    }

    mbedtls_sha256_context sha;
    bool shaStarted = false;
    if(verifySha) {
        mbedtls_sha256_init(&sha);
        shaStarted = mbedtls_sha256_starts(&sha, 0) == 0;
        if(!shaStarted) {
            free(buffer);
            (void)esp_ota_abort(handle);
            mbedtls_sha256_free(&sha);
            finish(result, false, "sha256 init failed");
            return false;
        }
    }

    uint32_t written = 0;
    uint32_t lastProgressAt = millis();
    bool ok = true;
    String message;

    while((client.connected() || stream.available() > 0) && written < static_cast<uint32_t>(contentLength)) {
        const int available = stream.available();
        if(available <= 0) {
            if(static_cast<uint32_t>(millis() - lastProgressAt) > 30000UL) {
                ok = false;
                message = "download timeout";
                break;
            }
            delay(2);
            yield();
            continue;
        }

        const uint32_t remaining = static_cast<uint32_t>(contentLength) - written;
        const size_t toRead =
            static_cast<size_t>(min<uint32_t>(min<uint32_t>(available, kOtaDownloadBufferSize), remaining));
        const int bytesRead = stream.readBytes(buffer, toRead);
        if(bytesRead <= 0) {
            ok = false;
            message = "stream read failed";
            break;
        }

        if(verifySha && mbedtls_sha256_update(&sha, buffer, static_cast<size_t>(bytesRead)) != 0) {
            ok = false;
            message = "sha256 update failed";
            break;
        }

        err = esp_ota_write(handle, buffer, static_cast<size_t>(bytesRead));
        if(err != ESP_OK) {
            ok = false;
            message = String("ota write err=") + String(static_cast<int>(err));
            break;
        }
        written += static_cast<uint32_t>(bytesRead);
        lastProgressAt = millis();
        if(result != nullptr) {
            result->written = written;
        }
        if(options.progress != nullptr) {
            options.progress(written, static_cast<uint32_t>(contentLength), options.progressContext);
        }
        yield();
    }

    if(ok && written != static_cast<uint32_t>(contentLength)) {
        ok = false;
        message = String("short download ") + String(written) + "/" + String(contentLength);
    }

    String actualSha;
    if(ok && verifySha) {
        uint8_t digest[32] = {};
        if(mbedtls_sha256_finish(&sha, digest) != 0) {
            ok = false;
            message = "sha256 finish failed";
        } else {
            actualSha = sha256ToHex(digest);
            if(result != nullptr) {
                result->sha256 = actualSha;
            }
            if(actualSha != expectedSha) {
                ok = false;
                message = "sha256 mismatch";
            }
        }
    }

    if(ok) {
        err = esp_ota_end(handle);
        if(err != ESP_OK) {
            ok = false;
            message = String("ota end err=") + String(static_cast<int>(err));
        }
    } else {
        (void)esp_ota_abort(handle);
    }

    if(shaStarted) {
        mbedtls_sha256_free(&sha);
    }
    if(result != nullptr) {
        result->written = written;
        result->verified = ok && verifySha;
    }
    free(buffer);
    if(ok) {
        message = String("downloaded ") + partition->label + " " + offsetHex(partition->address) +
                  " bytes=" + String(written);
        if(verifySha) {
            message += " sha256=ok";
        }
        if(options.version.length() > 0) {
            message += " version=" + options.version;
        }
    }
    finish(result, ok, message);
    return ok;
}

inline bool downloadHttpsToNextSlot(const String& url,
                                    const esp_partition_t* partition,
                                    const DownloadOptions& options,
                                    Result* result) {
    WiFiClientSecure client;
    String headers;
    String effectiveUrl;
    if(!openHttpsGet(url, "application/octet-stream", &client, &headers, &effectiveUrl, result)) {
        return false;
    }

    const int statusCode = parseStatusCode(headers);
    if(statusCode != 200) {
        client.stop();
        finish(result, false, String("http ") + String(statusCode));
        return false;
    }
    const int contentLength = parseContentLength(headers);
    const bool ok = writeOtaBody(client, client, partition, contentLength, options, result);
    client.stop();
    return ok;
}

inline bool writeRawPartitionBody(Stream& stream,
                                  Client& client,
                                  const esp_partition_t* partition,
                                  uint32_t partitionOffset,
                                  int contentLength,
                                  const DownloadOptions& options,
                                  Result* result) {
    if(partition == nullptr || contentLength <= 0) {
        finish(result, false, "invalid staging partition");
        return false;
    }
    const uint32_t total = static_cast<uint32_t>(contentLength);
    if(options.expectedSize > 0 && total != options.expectedSize) {
        finish(result, false, String("size mismatch ") + total + "/" + options.expectedSize);
        return false;
    }
    if(partitionOffset > partition->size || total > partition->size - partitionOffset) {
        finish(result, false, "staging partition too small");
        return false;
    }
    const String expectedSha = normalizeSha256(options.expectedSha256);
    if(!isSha256Hex(expectedSha)) {
        finish(result, false, "bad manifest sha256");
        return false;
    }
    constexpr uint32_t kSectorSize = 4096;
    if((partitionOffset % kSectorSize) != 0) {
        finish(result, false, "staging offset not aligned");
        return false;
    }
    const uint32_t eraseSize = (total + kSectorSize - 1) & ~(kSectorSize - 1);
    if(esp_partition_erase_range(partition, partitionOffset, eraseSize) != ESP_OK) {
        finish(result, false, "staging erase failed");
        return false;
    }

    uint8_t* buffer = static_cast<uint8_t*>(malloc(kOtaDownloadBufferSize));
    if(buffer == nullptr) {
        finish(result, false, "staging buffer alloc failed");
        return false;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if(mbedtls_sha256_starts(&sha, 0) != 0) {
        mbedtls_sha256_free(&sha);
        free(buffer);
        finish(result, false, "sha256 init failed");
        return false;
    }
    uint32_t written = 0;
    uint32_t lastProgressAt = millis();
    bool ok = true;
    String message;
    while((client.connected() || stream.available() > 0) && written < total) {
        const int available = stream.available();
        if(available <= 0) {
            if(static_cast<uint32_t>(millis() - lastProgressAt) > 30000UL) {
                ok = false;
                message = "download timeout";
                break;
            }
            delay(2);
            yield();
            continue;
        }
        const size_t toRead = static_cast<size_t>(min<uint32_t>(
            min<uint32_t>(available, kOtaDownloadBufferSize), total - written));
        const int bytesRead = stream.readBytes(buffer, toRead);
        if(bytesRead <= 0 ||
           mbedtls_sha256_update(&sha, buffer, static_cast<size_t>(bytesRead)) != 0 ||
           esp_partition_write(partition, partitionOffset + written, buffer,
                               static_cast<size_t>(bytesRead)) != ESP_OK) {
            ok = false;
            message = "staging write failed";
            break;
        }
        written += static_cast<uint32_t>(bytesRead);
        lastProgressAt = millis();
        if(result != nullptr) result->written = written;
        if(options.progress != nullptr) options.progress(written, total, options.progressContext);
        yield();
    }
    if(ok && written != total) {
        ok = false;
        message = String("short download ") + written + "/" + total;
    }
    uint8_t digest[32] = {};
    if(ok && mbedtls_sha256_finish(&sha, digest) != 0) {
        ok = false;
        message = "sha256 finish failed";
    }
    mbedtls_sha256_free(&sha);
    const String actualSha = ok ? sha256ToHex(digest) : String();
    if(ok && actualSha != expectedSha) {
        ok = false;
        message = "sha256 mismatch";
    }
    free(buffer);
    if(result != nullptr) {
        result->expected = total;
        result->written = written;
        result->verified = ok;
        result->sha256 = actualSha;
        result->version = options.version;
        result->slotLabel = partition->label;
        result->slotOffset = offsetHex(partition->address + partitionOffset);
    }
    if(ok) message = String("staged ") + written + " bytes sha256=ok";
    finish(result, ok, message);
    return ok;
}

inline bool downloadHttpsToPartition(const String& url,
                                     const esp_partition_t* partition,
                                     uint32_t partitionOffset,
                                     const DownloadOptions& options,
                                     Result* result) {
    if(result != nullptr) {
        *result = Result();
        result->imageUrl = url;
    }
    WiFiClientSecure client;
    String headers;
    String effectiveUrl;
    if(!openHttpsGet(url, "application/octet-stream", &client, &headers, &effectiveUrl, result)) {
        return false;
    }
    const int statusCode = parseStatusCode(headers);
    if(statusCode != 200) {
        client.stop();
        finish(result, false, String("http ") + statusCode);
        return false;
    }
    const bool ok = writeRawPartitionBody(client, client, partition, partitionOffset,
                                          parseContentLength(headers), options, result);
    client.stop();
    return ok;
}

inline String jsonString(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if(!cJSON_IsString(item) || item->valuestring == nullptr) {
        return String();
    }
    return String(item->valuestring);
}

inline uint32_t jsonUint32(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if(!cJSON_IsNumber(item) || item->valuedouble < 0.0) {
        return 0;
    }
    return static_cast<uint32_t>(item->valuedouble);
}

inline bool jsonBool(const cJSON* object, const char* key, bool* out) {
    if(out == nullptr) {
        return false;
    }
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if(!cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

inline bool parseManifestForTarget(const String& manifestUrl,
                          const String& payload,
                          const String& expectedRole,
                          const String& expectedEnv,
                          ManifestSpec* spec,
                          Result* result) {
    if(spec == nullptr) {
        return false;
    }
    *spec = ManifestSpec();

    cJSON* root = cJSON_ParseWithLength(payload.c_str(), payload.length());
    if(root == nullptr) {
        finish(result, false, "manifest json parse failed");
        return false;
    }

    const String schema = jsonString(root, "schema");
    if(schema != "sg98-ota-manifest-v1" && schema != "sg98-flash-manifest-v2") {
        cJSON_Delete(root);
        finish(result, false, "unsupported manifest schema");
        return false;
    }

    const cJSON* application = cJSON_GetObjectItemCaseSensitive(root, "application");
    if(!cJSON_IsObject(application)) {
        cJSON_Delete(root);
        finish(result, false, "manifest missing application");
        return false;
    }

    spec->version = jsonString(root, "version");
    spec->gitSha = jsonString(root, "gitSha");
    spec->generatedAtUtc = jsonString(root, "generatedAtUtc");
    spec->gitDirtyPresent = jsonBool(root, "gitDirty", &spec->gitDirty);
    spec->role = jsonString(root, "role");
    spec->envName = jsonString(root, "envName");
    if(spec->envName.length() == 0) {
        spec->envName = jsonString(root, "buildEnv");
    }
    if(spec->envName.length() == 0) {
        spec->envName = jsonString(root, "target");
    }

    spec->imageUrl = jsonString(application, "url");
    if(spec->imageUrl.length() == 0) {
        spec->imageUrl = jsonString(application, "fileName");
    }
    spec->sha256 = normalizeSha256(jsonString(application, "sha256"));
    spec->size = jsonUint32(application, "size");
    const String offset = jsonString(application, "offset");
    cJSON_Delete(root);

    if(spec->role.length() == 0 || spec->envName.length() == 0) {
        finish(result, false, "manifest missing role/env");
        return false;
    }
    if(spec->role != expectedRole) {
        finish(result, false, String("manifest role ") + spec->role + " != " + expectedRole);
        return false;
    }
    if(spec->envName != expectedEnv) {
        finish(result, false, String("manifest env ") + spec->envName + " != " + expectedEnv);
        return false;
    }
    if(spec->imageUrl.length() == 0) {
        finish(result, false, "manifest missing image url");
        return false;
    }
    if(spec->size == 0) {
        finish(result, false, "manifest missing image size");
        return false;
    }
    if(!isSha256Hex(spec->sha256)) {
        finish(result, false, "manifest missing sha256");
        return false;
    }
    if(offset.length() > 0 && offset != "0x10000" && offset != "0X10000") {
        finish(result, false, "manifest application offset unsupported");
        return false;
    }

    spec->imageUrl = resolveUrlReference(stripFragment(manifestUrl), spec->imageUrl);
    if(!spec->imageUrl.startsWith("https://")) {
        finish(result, false, "manifest image url must be https");
        return false;
    }
    return true;
}

inline bool parseManifest(const String& manifestUrl,
                          const String& payload,
                          ManifestSpec* spec,
                          Result* result) {
    return parseManifestForTarget(manifestUrl, payload, sg98_version::role(),
                                  sg98_version::buildEnv(), spec, result);
}

inline bool fetchManifestSpec(const String& manifestUrl, ManifestSpec* spec, Result* result) {
    if(result != nullptr) {
        *result = Result();
        result->fromManifest = true;
    }
    if(manifestUrl.length() == 0) {
        finish(result, false, "missing manifest url");
        return false;
    }
    if(WiFi.status() != WL_CONNECTED || WiFi.localIP()[0] == 0) {
        finish(result, false, "wifi not connected");
        return false;
    }

    String payload;
    if(!fetchHttpsString(manifestUrl, "application/json", kMaxManifestBytes, &payload, result)) {
        return false;
    }

    const bool ok = parseManifest(manifestUrl, payload, spec, result);
    if(ok && result != nullptr && spec != nullptr) {
        result->ok = true;
        result->version = spec->version;
        result->sha256 = spec->sha256;
        result->imageUrl = spec->imageUrl;
        result->expected = spec->size;
        result->message = "manifest ok";
    }
    return ok;
}

inline bool fetchManifestSpecForTarget(const String& manifestUrl,
                                       const String& expectedRole,
                                       const String& expectedEnv,
                                       ManifestSpec* spec,
                                       Result* result) {
    if(result != nullptr) {
        *result = Result();
        result->fromManifest = true;
    }
    if(manifestUrl.length() == 0 || WiFi.status() != WL_CONNECTED || WiFi.localIP()[0] == 0) {
        finish(result, false, manifestUrl.length() == 0 ? "missing manifest url" : "wifi not connected");
        return false;
    }
    String payload;
    if(!fetchHttpsString(manifestUrl, "application/json", kMaxManifestBytes, &payload, result)) return false;
    const bool ok = parseManifestForTarget(manifestUrl, payload, expectedRole, expectedEnv, spec, result);
    if(ok && result != nullptr && spec != nullptr) {
        result->ok = true;
        result->version = spec->version;
        result->sha256 = spec->sha256;
        result->imageUrl = spec->imageUrl;
        result->expected = spec->size;
        result->message = "manifest ok";
    }
    return ok;
}

inline bool downloadToNextSlot(const String& url, const DownloadOptions& options, Result* result) {
    if(result != nullptr) {
        *result = Result();
    }
    if(url.length() == 0) {
        finish(result, false, "missing url");
        return false;
    }
    if(WiFi.status() != WL_CONNECTED || WiFi.localIP()[0] == 0) {
        finish(result, false, "wifi not connected");
        return false;
    }

    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    if(partition == nullptr) {
        finish(result, false, "no inactive ota slot");
        return false;
    }
    if(result != nullptr) {
        result->slotLabel = partition->label;
        result->slotOffset = offsetHex(partition->address);
        result->imageUrl = url;
    }

    if(url.startsWith("https://")) {
        return downloadHttpsToNextSlot(url, partition, options, result);
    }

    HTTPClient http;
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if(!http.begin(url)) {
        finish(result, false, "http begin failed");
        return false;
    }

    const int code = http.GET();
    if(code != HTTP_CODE_OK) {
        const String message = String("http ") + String(code);
        http.end();
        finish(result, false, message);
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    const int contentLength = http.getSize();
    const bool ok = stream != nullptr && writeOtaBody(*stream, *stream, partition, contentLength, options, result);
    if(stream == nullptr) {
        finish(result, false, "http stream failed");
    }
    http.end();
    return ok;
}

inline bool downloadToNextSlot(const String& url, Result* result) {
    return downloadToNextSlot(url, DownloadOptions(), result);
}

inline bool downloadManifestToNextSlot(const String& manifestUrl, Result* result) {
    if(result != nullptr) {
        *result = Result();
        result->fromManifest = true;
    }
    if(manifestUrl.length() == 0) {
        finish(result, false, "missing manifest url");
        return false;
    }
    if(WiFi.status() != WL_CONNECTED || WiFi.localIP()[0] == 0) {
        finish(result, false, "wifi not connected");
        return false;
    }

    ManifestSpec spec;
    if(!fetchManifestSpec(manifestUrl, &spec, result)) {
        return false;
    }

    DownloadOptions options;
    options.expectedSize = spec.size;
    options.expectedSha256 = spec.sha256;
    options.version = spec.version;

    const bool ok = downloadToNextSlot(spec.imageUrl, options, result);
    if(result != nullptr) {
        result->fromManifest = true;
        result->imageUrl = spec.imageUrl;
        result->version = spec.version;
    }
    return ok;
}

}  // namespace sg98_ota_download
