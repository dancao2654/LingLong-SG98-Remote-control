#include "BLEScale.h"

#include <string>

BLEScale::BLEScale(bool debug)
    : _clientCallbacks(this),
      _scanCallbacks(this) {
    _debug = debug;
}

bool BLEScale::manage(bool connect, bool maintain, String mac) {
    static unsigned long lastCall = 0;

    if(lastCall + 50 > millis()) {
        return false;
    }
    lastCall = millis();

    NimBLEScan* pScan = NimBLEDevice::getScan();
    static bool scanConfigured = false;
    if(!scanConfigured) {
        pScan->setActiveScan(false);
        pScan->setScanCallbacks(&_scanCallbacks, false);
        scanConfigured = true;
    }

    _connected = (_pClient != nullptr) && _pClient->isConnected();
    connectMac = mac.length() ? mac : "";

    if(!_connected && !_isConnecting && connect && !pScan->isScanning()) {
        pScan->start(5000);
    }

    if(maintain && _connected) {
        if(_type == ScaleType::ACAIA_NEW || _type == ScaleType::ACAIA_OLD) {
            sendHeartbeatAcaia();
        }
    }

    if(!maintain && !connect) {
        disconnect();
    }

    if(doConnect && connect) {
        doConnect = false;
        if(connectToServer()) {
            if(_debug) {
                Serial.println("Success! we should now be getting notifications");
            }
            _connected = true;
            _isConnecting = false;
            return true;
        }

        if(_debug) {
            Serial.println("Failed to connect");
        }
        _connected = false;
        _isConnecting = false;
        return false;
    }

    return false;
}

void BLEScale::disconnect() {
    if(_pClient && _pClient->isConnected()) {
        _pClient->disconnect();
    }

    _pChrREAD = nullptr;
    _pChrWRITE = nullptr;
    if(advDevice) {
        delete advDevice;
        advDevice = nullptr;
    }
    _type = ScaleType::UNKNOWN;
    _connected = false;
    _isConnecting = false;
}

bool BLEScale::tare() {
    if(!_connected || !_pChrWRITE) {
        return false;
    }

    if(_type == ScaleType::ACAIA_OLD) {
        const byte tareAcaia[6] = {0xef, 0xdd, 0x04, 0x00, 0x00, 0x00};
        return _pChrWRITE->writeValue(tareAcaia, sizeof(tareAcaia), false);
    }

    if(_type == ScaleType::ACAIA_NEW) {
        const byte tareRequest[6] = {0xef, 0xdd, 0x04, 0x00, 0x00, 0x00};
        return _pChrWRITE->writeValue(tareRequest, sizeof(tareRequest), true);
    }

    if(_type == ScaleType::BOOKOO) {
        const uint8_t payload[6] = {0x03, 0x0a, 0x01, 0x00, 0x00, 0x08};
        sendMessageBookoo(payload, sizeof(payload));
        return true;
    }

    if(_type == ScaleType::FELIC) {
        const uint8_t tareCmd[] = {0x54};
        return _pChrWRITE->writeValue(tareCmd, sizeof(tareCmd), true);
    }

    return false;
}

bool BLEScale::startTimer() {
    if(!_connected || !_pChrWRITE) {
        return false;
    }

    if(_type == ScaleType::ACAIA_NEW || _type == ScaleType::ACAIA_OLD) {
        return false;
    }

    if(_type == ScaleType::BOOKOO) {
        const uint8_t payload[6] = {0x03, 0x0a, 0x04, 0x00, 0x00, 0x0a};
        sendMessageBookoo(payload, sizeof(payload));
        return true;
    }

    return false;
}

bool BLEScale::resetTimer() {
    if(!_connected || !_pChrWRITE) {
        return false;
    }

    if(_type == ScaleType::ACAIA_NEW || _type == ScaleType::ACAIA_OLD) {
        return false;
    }

    if(_type == ScaleType::BOOKOO) {
        const uint8_t payload[6] = {0x03, 0x0a, 0x06, 0x00, 0x00, 0x0c};
        sendMessageBookoo(payload, sizeof(payload));
        return true;
    }

    return false;
}

bool BLEScale::stopTimer() {
    if(!_connected || !_pChrWRITE) {
        return false;
    }

    if(_type == ScaleType::ACAIA_NEW || _type == ScaleType::ACAIA_OLD) {
        return false;
    }

    if(_type == ScaleType::BOOKOO) {
        const uint8_t payload[6] = {0x03, 0x0a, 0x05, 0x00, 0x00, 0x0d};
        sendMessageBookoo(payload, sizeof(payload));
        return true;
    }

    return false;
}

bool BLEScale::newWeightAvailable() {
    const bool available = _weightAvailable;
    _weightAvailable = false;
    return available;
}

bool BLEScale::isConnected() {
    return _connected;
}

bool BLEScale::isConnecting() {
    return _isConnecting;
}

float BLEScale::getWeight() {
    return _weight;
}

unsigned long BLEScale::getTimer() {
    return _timer;
}

int BLEScale::getBatteryLevel() {
    return _batteryLevel;
}

bool BLEScale::connectToServer() {
    if(!advDevice) {
        return false;
    }

    NimBLEClient* client = nullptr;

    if(NimBLEDevice::getCreatedClientCount()) {
        client = NimBLEDevice::getClientByPeerAddress(advDevice->getAddress());
        if(client) {
            if(!client->connect(advDevice, false)) {
                if(_debug) {
                    Serial.println("Reconnect failed");
                }
                return false;
            }
            if(_debug) {
                Serial.println("Reconnected existing client");
            }
        } else {
            client = NimBLEDevice::getDisconnectedClient();
        }
    }

    if(!client) {
        if(NimBLEDevice::getCreatedClientCount() >= 3) {
            if(_debug) {
                Serial.println("Max clients reached - no more connections available");
            }
            return false;
        }

        client = NimBLEDevice::createClient();
        if(!client) {
            Serial.println("createClient() returned nullptr (init missing or OOM)");
            return false;
        }

        NimBLEDevice::setMTU(23);
        client->setClientCallbacks(&_clientCallbacks, false);
        client->setConnectionParams(12, 24, 0, 400);
        client->setConnectTimeout(450);

        if(_debug) {
            Serial.println("New client created");
        }
    } else {
        client->setClientCallbacks(&_clientCallbacks, false);
    }

    if(_debug) {
        Serial.printf("Trying to connect. AddrType=%d\n", advDevice->getAddressType());
    }

    if(!client->connect(advDevice)) {
        if(_debug) {
            Serial.println("Connect failed with advertised-device overload");
        }

        if(!client->connect(advDevice->getAddress(), advDevice->getAddressType())) {
            if(_debug) {
                Serial.println("Connect failed (explicit type). Deleting client.");
            }
            NimBLEDevice::deleteClient(client);
            return false;
        }
    }

    _pClient = client;

    if(_pClient->getService("49535343-fe7d-4ae5-8fa9-9fafd205e455")) {
        _type = ScaleType::ACAIA_NEW;
        performHandshakeAcaia(false);
    } else if(_pClient->getService("1820")) {
        _type = ScaleType::ACAIA_OLD;
        performHandshakeAcaia(true);
    } else if(_pClient->getService("0FFE")) {
        _type = ScaleType::BOOKOO;
        performHandshakeBookoo();
    } else if(_pClient->getService("FFE0")) {
        _type = ScaleType::FELIC;
        performHandshakeFelicita();
    } else {
        _type = ScaleType::UNKNOWN;
    }

    if(_type == ScaleType::UNKNOWN) {
        return false;
    }

    {
        std::string tmpName;
        if(_type == ScaleType::FELIC) {
            tmpName = "Felicita";
        } else if(advDevice) {
            tmpName = advDevice->getName();
        }
        connected_name = tmpName.c_str();
    }
    {
        if(_pClient) {
            std::string tmpMac = _pClient->getPeerAddress().toString();
            connected_mac = tmpMac.c_str();
        }
    }

    if(_debug) {
        Serial.print("Connected to: ");
        Serial.println(connected_name.c_str());
        Serial.print("with addr: ");
        Serial.println(connected_mac.c_str());
    }

    return true;
}

void BLEScale::notifyCB(NimBLERemoteCharacteristic* /*chr*/, uint8_t* data,
                        size_t len, bool /*isNotify*/, void* ctx) {
    BLEScale* self = static_cast<BLEScale*>(ctx);
    if(self) {
        self->handleNotification(data, len);
    }
}

void BLEScale::handleNotification(uint8_t* data, size_t length) {
    if(_type == ScaleType::ACAIA_NEW) {
        dataBuffer.insert(dataBuffer.end(), data, data + length);
        bool more = true;
        while(more) {
            more = decodeAndHandleAcaia();
        }
        return;
    }

    static const float pow10[] = {1, 10, 100, 1000, 10000, 100000};
    if(_type == ScaleType::ACAIA_OLD) {
        if(length == 10 || length == 14) {
            const uint16_t rawWeight = ((data[3] & 0xff) << 8) | (data[2] & 0xff);
            const byte scaleIndex = data[6];
            const float scale = (scaleIndex < sizeof(pow10) / sizeof(pow10[0])) ? pow10[scaleIndex] : 1.0f;
            _weight = rawWeight / scale * ((data[7] & 0x02) ? -1 : 1);
            _weightAvailable = true;
            if(_debug) {
                Serial.println(_weight);
            }
        }
    } else if(_type == ScaleType::BOOKOO) {
        _weight = decodeWeightBookoo(data, length);
        _weightAvailable = true;
    } else if(_type == ScaleType::FELIC) {
        if(length < 18) {
            return;
        }
        const int32_t w = parseWeightFelicita(data, length);
        _weight = static_cast<float>(w) / 100.0f;
        _weightAvailable = true;
    }
}

void BLEScale::cleanupJunkData(std::vector<uint8_t>& buf) {
    int start = 0;
    while(start < static_cast<int>(buf.size()) - 1 &&
          !(buf[start] == 0xEF && buf[start + 1] == 0xDD)) {
        start++;
    }
    if(start > 0) {
        buf.erase(buf.begin(), buf.begin() + start);
    }

    if(buf.size() == 1 && buf[0] != 0xEF) {
        buf.clear();
    }
}

void BLEScale::performHandshakeAcaia(bool oldVersion) {
    if(!_pClient) {
        return;
    }

    if(oldVersion) {
        NimBLERemoteService* svc = _pClient->getService("1820");
        if(!svc) {
            if(_debug) {
                Serial.println("ACAIA OLD service not found");
            }
            return;
        }

        NimBLERemoteCharacteristic* chr = svc->getCharacteristic("2a80");
        if(!chr) {
            if(_debug) {
                Serial.println("ACAIA OLD char 2a80 not found");
            }
            return;
        }

        _pChrREAD = chr;
        _pChrWRITE = chr;

        if(_pChrREAD->canWriteNoResponse() && _debug) {
            Serial.println("Succes pChrREAD -> canWriteNoResponse()");
        }

        if(_pChrREAD->canNotify()) {
            subscribeNotifications(_pChrREAD);
            if(_debug) {
                Serial.println("Subscribed to ACAIA OLD notifications.");
            }
        } else if(_debug) {
            Serial.println("ACAIA OLD: 0x2a80 cannot notify");
        }

        if(!_pChrWRITE->canWriteNoResponse() && _debug) {
            Serial.println("ACAIA OLD: 0x2a80 does not support write-no-response");
        }

        const byte identifyOld[20] = {
            0xef, 0xdd, 0x0b, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x9a, 0x6d
        };
        const byte notificationRequest[14] = {
            0xef, 0xdd, 0x0c, 0x09, 0x00, 0x01, 0x01, 0x02,
            0x02, 0x05, 0x03, 0x04, 0x15, 0x06
        };

        if(_pChrWRITE) {
            _pChrWRITE->writeValue(identifyOld, sizeof(identifyOld), false);
            _pChrWRITE->writeValue(notificationRequest, sizeof(notificationRequest), false);
        }
        return;
    }

    NimBLERemoteService* svc = _pClient->getService("49535343-fe7d-4ae5-8fa9-9fafd205e455");
    if(!svc) {
        if(_debug) {
            Serial.println("ACAIA NEW service not found");
        }
        return;
    }

    _pChrREAD = svc->getCharacteristic("49535343-1e4d-4bd9-ba61-23c647249616");
    _pChrWRITE = svc->getCharacteristic("49535343-8841-43f4-a8d4-ecbe34729bb3");

    if(_pChrREAD) {
        subscribeNotifications(_pChrREAD);
    } else {
        if(_debug) {
            Serial.println("ACAIA NEW read characteristic not found");
        }
        return;
    }

    delay(10);
    if(!_pChrWRITE) {
        if(_debug) {
            Serial.println("ACAIA NEW write characteristic not found");
        }
        return;
    }

    Serial.println("Sending ID and notification request...");
    const byte identifyNew[20] = {
        0xef, 0xdd, 0x0b, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d,
        0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x68, 0x3b
    };
    _pChrWRITE->writeValue(identifyNew, sizeof(identifyNew), true);
}

void BLEScale::sendMessageAcaia(uint8_t msgType, const uint8_t* payload, size_t length, bool waitResponse) {
    uint8_t header[3] = {0xEF, 0xDD, msgType};
    uint8_t len = static_cast<uint8_t>(length);
    uint16_t checksum = 0;
    for(size_t i = 0; i < length; i++) {
        checksum += payload[i];
    }

    std::vector<uint8_t> msg;
    msg.insert(msg.end(), header, header + 3);
    msg.push_back(len);
    msg.insert(msg.end(), payload, payload + length);
    msg.push_back(checksum & 0xFF);
    msg.push_back((checksum >> 8) & 0xFF);
    if(_pChrWRITE) {
        _pChrWRITE->writeValue(msg.data(), msg.size(), !waitResponse);
    }
}

void BLEScale::sendHeartbeatAcaia() {
    const unsigned long now = millis();
    if(now - lastHeartbeat < 2750) {
        return;
    }

    if(_type == ScaleType::ACAIA_OLD) {
        const uint8_t hbOld[7] = {0xef, 0xdd, 0x00, 0x02, 0x00, 0x02, 0x00};
        if(_pChrWRITE) {
            const bool ok = _pChrWRITE->writeValue(hbOld, sizeof(hbOld), false);
            if(_debug) {
                Serial.printf("Sent ACAIA OLD heartbeat [%s]\n", ok ? "OK" : "FAIL");
            }
        } else if(_debug) {
            Serial.println("ACAIA OLD heartbeat skipped, WRITE char invalid");
        }

        lastHeartbeat = now;
        return;
    }

    if(_type == ScaleType::ACAIA_NEW) {
        const byte heartbeatSystem[7] = {0xef, 0xdd, 0x00, 0x02, 0x00, 0x02, 0x00};
        const byte notificationRequest[14] = {
            0xef, 0xdd, 0x0c, 0x09, 0x00, 0x01, 0x01, 0x02,
            0x02, 0x05, 0x03, 0x04, 0x15, 0x06
        };

        int score = 0;
        if(_pChrWRITE && _pChrWRITE->writeValue(heartbeatSystem, sizeof(heartbeatSystem), true)) {
            score++;
        }
        if(_pChrWRITE && _pChrWRITE->writeValue(notificationRequest, sizeof(notificationRequest), true)) {
            score++;
        }

        if(_debug) {
            Serial.printf("Sent ACAIA NEW heartbeat [%s]\n", score == 2 ? "OK" : "FAIL");
        } else if(!_pChrWRITE && _debug) {
            Serial.println("ACAIA NEW heartbeat skipped, WRITE char invalid");
        }

        lastHeartbeat = now;
        return;
    }
}

float BLEScale::decodeWeightAcaiaEvent(const uint8_t* w) {
    float value = float((w[1] << 8) | w[0]);
    const uint8_t scaling = w[4];

    switch(scaling) {
        case 1: value /= 10.f; break;
        case 2: value /= 100.f; break;
        case 3: value /= 1000.f; break;
        case 4: value /= 10000.f; break;
        default: return 0.f;
    }

    if(w[5] & 0x02) {
        value = -value;
    }
    return value;
}

unsigned long BLEScale::decodeTimeAcaiaEvent(const uint8_t* t) {
    return static_cast<unsigned long>(t[0] * 60 + t[1]);
}

std::pair<uint8_t, uint8_t> BLEScale::checksumAcaia(const uint8_t* payload, size_t len) {
    uint8_t cksum1 = 0;
    uint8_t cksum2 = 0;

    for(size_t i = 0; i < len; i++) {
        if(i % 2 == 0) {
            cksum1 += payload[i];
        } else {
            cksum2 += payload[i];
        }
    }

    return {static_cast<uint8_t>(cksum2), static_cast<uint8_t>(cksum1)};
}

bool BLEScale::decodeAndHandleAcaia() {
    static constexpr size_t kHeaderLen = 3;
    static constexpr size_t kChecksumLen = 2;
    static constexpr size_t kMinFrame = kHeaderLen + 1 + kChecksumLen;

    cleanupJunkData(dataBuffer);
    if(dataBuffer.size() < kMinFrame) {
        return false;
    }

    if(dataBuffer.size() < 4) {
        return false;
    }

    const uint8_t payloadLen = dataBuffer[3];
    const size_t frameLen = kHeaderLen + 1 + payloadLen + kChecksumLen;
    if(dataBuffer.size() < frameLen) {
        return false;
    }

    if(payloadLen > 64) {
        dataBuffer.erase(dataBuffer.begin());
        return dataBuffer.size() >= kMinFrame;
    }

    const uint8_t* frame = dataBuffer.data();
    const uint8_t* payload = frame + 4;

    const auto ck = checksumAcaia(payload, payloadLen);
    const uint8_t c1 = frame[frameLen - 2];
    const uint8_t c2 = frame[frameLen - 1];

    if(ck.second != c1) {
        if(_debug) {
            Serial.printf("Checksum fail: calc[%02X %02X] recv[%02X %02X]\n", ck.first, ck.second, c1, c2);
        }
        dataBuffer.erase(dataBuffer.begin());
        return dataBuffer.size() >= kMinFrame;
    }

    const uint8_t msgType = frame[2];
    if(msgType == 0x0C) {
        handleAcaiaEventPayload(payload, payloadLen);
    } else if(msgType == 0x02) {
        handleAcaiaStatusPayload(payload, payloadLen);
    } else if(_debug) {
        Serial.printf("Unknown msgType %02X len=%u\n", msgType, payloadLen);
    }

    dataBuffer.erase(dataBuffer.begin(), dataBuffer.begin() + frameLen);
    return dataBuffer.size() >= kMinFrame;
}

void BLEScale::handleAcaiaEventPayload(const uint8_t* p, size_t len) {
    if(len < 2) {
        return;
    }

    const uint8_t eventType = p[0];
    const uint8_t* e = p + 1;

    switch(eventType) {
        case 0x05: {
            if(len < 8) {
                return;
            }
            const float v = decodeWeightAcaiaEvent(e);
            _weight = v;
            _weightAvailable = true;
            if(_debug) {
                Serial.printf("Weight: %.4f\n", v);
            }
            break;
        }
        case 0x07: {
            if(len < 5) {
                return;
            }
            _timer = decodeTimeAcaiaEvent(e);
            if(_debug) {
                Serial.printf("Timer: %lus\n", _timer);
            }
            break;
        }
        case 0x06: {
            if(len >= 3) {
                _batteryLevel = static_cast<int>(e[0] & 0x7F);
            }
            break;
        }
        default:
            if(_debug) {
                Serial.printf("Unknown event %02X\n", eventType);
            }
            break;
    }
}

void BLEScale::handleAcaiaStatusPayload(const uint8_t* p, size_t len) {
    if(len < 3) {
        return;
    }

    _batteryLevel = static_cast<int>(p[1] & 0x7F);
    if(_debug) {
        const char* units = (p[2] == 2 ? "g" : (p[2] == 5 ? "oz" : "?"));
        Serial.printf("Status: batt=%d%% units=%s\n", _batteryLevel, units);
    }
}

void BLEScale::performHandshakeBookoo() {
    NimBLERemoteService* svc = _pClient->getService("0FFE");
    if(!svc) {
        if(_debug) {
            Serial.println("BOOKOO service not found");
        }
        return;
    }

    _pChrREAD = svc->getCharacteristic("FF11");
    _pChrWRITE = svc->getCharacteristic("FF12");
    if(!_pChrREAD || !_pChrWRITE) {
        if(_debug) {
            Serial.println("BOOKOO chars missing");
        }
        return;
    }

    subscribeNotifications(_pChrREAD);
    sendNotificationRequestBookoo();
}

void BLEScale::sendMessageBookoo(const uint8_t* payload, size_t length, bool waitResponse) {
    if(_pChrWRITE) {
        _pChrWRITE->writeValue(payload, length, !waitResponse);
    }
}

void BLEScale::sendNotificationRequestBookoo() {
    const uint8_t payload1[] = {0x02, 0x00};
    sendMessageBookoo(payload1, sizeof(payload1));
    const uint8_t payload2[] = {0x00};
    sendMessageBookoo(payload2, sizeof(payload2));
}

void BLEScale::sendHeartbeatBookoo() {
    const uint8_t hb[] = {0, 0, 0, 0, 0, 0};
    sendMessageBookoo(hb, sizeof(hb));
}

float BLEScale::decodeWeightBookoo(const uint8_t* data, size_t len) {
    if(len < 10) {
        return 0.0f;
    }

    int32_t raw = (data[7] << 16) | (data[8] << 8) | data[9];
    if(data[6] == 45) {
        raw = -raw;
    }

    const float weight = raw / 100.0f;
    if(_debug) {
        Serial.printf("Bookoo weight decoded: %.2fg\n", weight);
    }
    return weight;
}

void BLEScale::performHandshakeFelicita() {
    NimBLERemoteService* svc = _pClient->getService("FFE0");
    if(!svc) {
        if(_debug) {
            Serial.println("FELICITA service not found");
        }
        return;
    }

    _pChrREAD = svc->getCharacteristic("FFE1");
    _pChrWRITE = svc->getCharacteristic("FFE1");
    if(!_pChrREAD || !_pChrWRITE) {
        if(_debug) {
            Serial.println("FELICITA chars missing");
        }
        return;
    }

    subscribeNotifications(_pChrREAD);
    const uint8_t identify[20] = {
        0xef, 0xdd, 0x0b, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x9a, 0x6d
    };
    _pChrWRITE->writeValue(identify, sizeof(identify), true);
    const uint8_t notificationRequest[14] = {
        0xef, 0xdd, 0x0c, 0x09, 0x00, 0x01, 0x01, 0x02,
        0x02, 0x05, 0x03, 0x04, 0x15, 0x06
    };
    _pChrWRITE->writeValue(notificationRequest, sizeof(notificationRequest), true);
}

int32_t BLEScale::parseWeightFelicita(const uint8_t* data, size_t length) {
    if(length < 18) {
        return 0;
    }

    const bool isNegative = (data[2] == 0x2D);
    for(int i = 3; i <= 8; ++i) {
        if(data[i] < '0' || data[i] > '9') {
            return 0;
        }
    }

    const int32_t v =
        (data[3] - '0') * 100000 +
        (data[4] - '0') * 10000 +
        (data[5] - '0') * 1000 +
        (data[6] - '0') * 100 +
        (data[7] - '0') * 10 +
        (data[8] - '0');

    return isNegative ? -v : v;
}

void BLEScale::subscribeNotifications(NimBLERemoteCharacteristic* chr) {
    if(chr && chr->canNotify()) {
        chr->subscribe(true, [this](NimBLERemoteCharacteristic* /*c*/, uint8_t* data,
                                    size_t len, bool /*isNotify*/) {
            handleNotification(data, len);
        });
    }
}

void BLEScale::ClientCallbacks::onConnect(NimBLEClient* pClient) {
    if(_parent) {
        _parent->_connected = true;
        _parent->_isConnecting = false;
        _parent->_pClient = pClient;
        Serial.println("Client connected");
    }
}

void BLEScale::ClientCallbacks::onDisconnect(NimBLEClient* /*pClient*/, int reason) {
    if(_parent) {
        _parent->_connected = false;
        _parent->_isConnecting = false;
        _parent->_pClient = nullptr;
        Serial.printf("Client disconnected, reason=%d\n", reason);
        _parent->_type = ScaleType::UNKNOWN;
        _parent->_pChrREAD = nullptr;
        _parent->_pChrWRITE = nullptr;
        if(_parent->advDevice) {
            delete _parent->advDevice;
            _parent->advDevice = nullptr;
        }
    }
}

void BLEScale::ScanCallbacks::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    if(!_parent) {
        return;
    }

    std::string devName = advertisedDevice->getName();
    std::string devAddr = advertisedDevice->getAddress().toString();

    if(_parent->_debug) {
        Serial.printf("Advertised Device found: %s\n", advertisedDevice->toString().c_str());
    }

    if(_parent->connectMac.length() > 0) {
        if(devAddr == _parent->connectMac.c_str()) {
            Serial.printf("Found scale by addr: %s\n", devAddr.c_str());
            NimBLEDevice::getScan()->stop();
            if(_parent->advDevice) {
                delete _parent->advDevice;
                _parent->advDevice = nullptr;
            }
            _parent->advDevice = new NimBLEAdvertisedDevice(*advertisedDevice);
            _parent->doConnect = true;
            _parent->_isConnecting = true;
            return;
        }
    } else {
        const bool hasFFE0 =
            advertisedDevice->haveServiceUUID() &&
            (advertisedDevice->isAdvertisingService(NimBLEUUID(static_cast<uint16_t>(0xFFE0))) ||
             advertisedDevice->isAdvertisingService(NimBLEUUID("0000FFE0-0000-1000-8000-00805F9B34FB")));

        if(hasFFE0 && devName.empty()) {
            Serial.println("Found scale by service UUID FFE0");
            NimBLEDevice::getScan()->stop();
            if(_parent->advDevice) {
                delete _parent->advDevice;
                _parent->advDevice = nullptr;
            }
            _parent->advDevice = new NimBLEAdvertisedDevice(*advertisedDevice);
            _parent->doConnect = true;
            _parent->_isConnecting = true;
            return;
        }

        if(devName.find("ACAIA") != std::string::npos ||
           devName.find("PEARL") != std::string::npos ||
           devName.find("PYXIS") != std::string::npos ||
           devName.find("CINCO") != std::string::npos ||
           devName.find("LUNAR") != std::string::npos ||
           devName.find("PROCH") != std::string::npos ||
           devName.find("FELIC") != std::string::npos ||
           devName.find("BOOKOO_SC") != std::string::npos) {
            Serial.printf("Found scale by name: %s\n", devName.c_str());
            NimBLEDevice::getScan()->stop();
            if(_parent->advDevice) {
                delete _parent->advDevice;
                _parent->advDevice = nullptr;
            }
            _parent->advDevice = new NimBLEAdvertisedDevice(*advertisedDevice);
            _parent->doConnect = true;
            _parent->_isConnecting = true;

            if(_parent->_debug) {
                Serial.print("End of OnResult -> Connecting?");
            }
        }
    }
}

void BLEScale::ScanCallbacks::onScanEnd(const NimBLEScanResults& results, int reason) {
    if(_parent && _parent->_debug) {
        Serial.printf("Scan ended. reason=%d, devices=%d\n", reason, results.getCount());
    }
}
