// controller.h - BMS Controller (BLE to ESP-NOW)
// Based on original controller.cpp
#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEUtils.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <Preferences.h>
#include "display_controller.h"

// ============== CONFIGURATION ==============
const char* ap_ssid = "JBD_BMS_Controller";
const char* ap_password = "12345678";

// ESP-NOW channel (must match on all devices)
#define ESPNOW_CHANNEL 1

// BMS BLE UUIDs
const BLEUUID SERVICE_UUID("0000ff00-0000-1000-8000-00805f9b34fb");
const BLEUUID CHAR_TX_UUID("0000ff01-0000-1000-8000-00805f9b34fb");
const BLEUUID CHAR_RX_UUID("0000ff02-0000-1000-8000-00805f9b34fb");

// ============== GLOBAL VARIABLES ==============
WebServer server(80);
Preferences prefs;

// BMS config
String BMS_MAC = "";
String BMS_NAME = "";
String savedBMSMac = "";
String savedBMSName = "";

// WiFi config
String wifi_ssid = "";
String wifi_password = "";
bool wifiConfigured = false;

// BLE
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pTxCharacteristic = nullptr;
BLERemoteCharacteristic* pRxCharacteristic = nullptr;
bool bmsConnected = false;

// BMS data buffer
uint8_t bmsResponse[512];
size_t bmsResponseLength = 0;
bool newDataReceived = false;

// Scan results
String availableBMS[20];
int bmsCount = 0;
bool isScanning = false;

// Hub config
uint8_t hub_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool hub_configured = false;

// Timing
unsigned long lastBMSRead = 0;
unsigned long lastDataSend = 0;
unsigned long lastDisplayUpdate = 0;

// ============== BMS DATA & DISPLAY INSTANCES ==============
BMSData bmsData;
LGFX_MGT020 tft;

// ============== HELPER FUNCTIONS ==============
uint16_t jbdChecksum(uint8_t* data, size_t len) {
  uint16_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += data[i];
  return ((~sum) + 1) & 0xFFFF;
}

// CRC для відповідей BMS: сумуємо від індексу 2 (статус) до len-4 (перед CRC)
uint16_t calculateResponseCRC(uint8_t* data, size_t len) {
  if (len < 7) return 0;
  uint16_t sum = 0;
  for (size_t i = 2; i < len - 3; i++) {
    sum += data[i];
  }
  return ((~sum) + 1) & 0xFFFF;
}

// CRC для команд: сумуємо від індексу 1 (команда) до len-4 (перед CRC)  
uint16_t calculateCommandCRC(uint8_t* data, size_t len) {
  if (len < 7) return 0;
  uint16_t sum = 0;
  for (size_t i = 1; i < len - 3; i++) {
    sum += data[i];
  }
  return ((~sum) + 1) & 0xFFFF;
}

bool verifyChecksum(uint8_t* data, size_t length) {
  if (length < 7 || data[length - 1] != 0x77) return false;
  // Для відповідей використовуємо calculateResponseCRC
  uint16_t calculated = calculateResponseCRC(data, length);
  uint16_t received = (data[length - 3] << 8) | data[length - 2];
  
  Serial.printf("CRC Debug: calculated=0x%04X, received=0x%04X\n", calculated, received);
  
  return calculated == received;
}

String getProtectionStatusDescription(uint16_t status) {
  if (status == 0) return "Normal";
  String desc = "";
  const char* protections[] = {
    "Cell overcharge", "Cell undercharge", "Pack overcharge", "Pack undercharge",
    "Charge overtemp", "Charge undertemp", "Discharge overtemp", "Discharge undertemp",
    "Charge overcurrent", "Discharge overcurrent", "Short circuit", "IC error", "MOS locked"
  };
  for (int i = 0; i < 13; i++) {
    if (status & (1 << i)) {
      if (desc.length() > 0) desc += ", ";
      desc += protections[i];
    }
  }
  return desc;
}

String getMOSFETStatusDescription(uint8_t status) {
  String charge = (status & 0x01) ? "On" : "Off";
  String discharge = (status & 0x02) ? "On" : "Off";
  return "Charge: " + charge + ", Discharge: " + discharge;
}

// ============== BLE CALLBACKS ==============
void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  static uint8_t fullResponse[512];
  static size_t fullLen = 0;
  static unsigned long lastPacket = 0;
  
  unsigned long now = millis();
  if (now - lastPacket > 100) {
    fullLen = 0;
    Serial.println("\n--- НОВА ВІДПОВІДЬ ---");
  }
  lastPacket = now;
  
  // Вивід отриманого пакету
  Serial.print("RX [");
  Serial.print(length);
  Serial.print("]: ");
  for (size_t i = 0; i < length; i++) {
    Serial.printf("%02X ", pData[i]);
  }
  Serial.println();
  
  if (fullLen + length < sizeof(fullResponse)) {
    memcpy(fullResponse + fullLen, pData, length);
    fullLen += length;
  }
  
  if (fullLen > 0 && fullResponse[fullLen - 1] == 0x77) {
    memcpy(bmsResponse, fullResponse, fullLen);
    bmsResponseLength = fullLen;
    newDataReceived = true;
    
    // Вивід повної відповіді
    Serial.print("FULL [");
    Serial.print(fullLen);
    Serial.print("]: ");
    for (size_t i = 0; i < fullLen; i++) {
      Serial.printf("%02X ", fullResponse[i]);
    }
    Serial.println(" [END]");
    
    fullLen = 0;
  }
}

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    String name = advertisedDevice.getName().c_str();
    String addr = advertisedDevice.getAddress().toString().c_str();
    if (name.length() > 0) {
      String info = addr + " - " + name;
      bool exists = false;
      for (int i = 0; i < bmsCount; i++) {
        if (availableBMS[i] == info) { exists = true; break; }
      }
      if (!exists && bmsCount < 20) availableBMS[bmsCount++] = info;
    }
  }
};

class MyClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) { bmsConnected = true; }
  void onDisconnect(BLEClient* pclient) { bmsConnected = false; }
};

// ============== BMS FUNCTIONS ==============
bool sendBMSCommand(uint8_t* cmd, size_t cmdLen, uint32_t timeout = 3000) {
  if (!bmsConnected || !pRxCharacteristic) {
    Serial.println("BMS не підключено");
    return false;
  }
  
  // Вивід команди
  Serial.print("\n>>> TX [");
  Serial.print(cmdLen);
  Serial.print("]: ");
  for (size_t i = 0; i < cmdLen; i++) {
    Serial.printf("%02X ", cmd[i]);
  }
  Serial.print(" | Reg: 0x");
  Serial.print(cmd[2], HEX);
  Serial.println();
  
  newDataReceived = false;
  bmsResponseLength = 0;
  pRxCharacteristic->writeValue(cmd, cmdLen, false);
  
  uint32_t start = millis();
  while (!newDataReceived && (millis() - start) < timeout) delay(10);
  
  if (!newDataReceived) {
    Serial.println("!!! Таймаут очікування відповіді");
    return false;
  }
  
  delay(100);
  
  // Вивід отриманої відповіді
  Serial.print("<<< RESP [");
  Serial.print(bmsResponseLength);
  Serial.print(" bytes]: ");
  for (size_t i = 0; i < bmsResponseLength && i < 20; i++) {
    Serial.printf("%02X ", bmsResponse[i]);
  }
  if (bmsResponseLength > 20) Serial.print("... ");
  Serial.printf("[CRC:%02X%02X]\n", bmsResponse[bmsResponseLength-3], bmsResponse[bmsResponseLength-2]);
  
  if (!verifyChecksum(bmsResponse, bmsResponseLength)) {
    // Перевіряємо, чи це відповідь на команду читання (0x03, 0x04, 0x05)
    if (bmsResponseLength > 4 && 
        (bmsResponse[1] == 0x03 || bmsResponse[1] == 0x04 || bmsResponse[1] == 0x05)) {
      Serial.println("УВАГА: Checksum помилка для читання, але продовжуємо обробку даних");
      // Не повертаємо помилку для команд читання
    } else {
      Serial.println("Помилка checksum");
      return false;
    }
  } else {
    Serial.println("Checksum OK");
  }
  
  return true;
}

bool parseBasicInfo(uint8_t* data, size_t length) {
  if (length < 15 || data[0] != 0xDD || data[1] != 0x03) {
    Serial.println("Некоректні дані базової інформації");
    return false;
  }

  uint8_t dataLength = data[3];
  Serial.print("Довжина даних: ");
  Serial.println(dataLength);
  
  // Карта байтів згідно документації JBD
  bmsData.totalVoltage = (float)((data[4] << 8) | data[5]) / 100.0; 
  Serial.printf("Загальна напруга [4-5]: %.2f В\n", bmsData.totalVoltage);
  
  int16_t rawCurrent = (data[6] << 8) | data[7];
  bmsData.current = (float)rawCurrent / 100.0; 
  Serial.printf("Струм [6-7]: %.2f А\n", bmsData.current);
  
  bmsData.capacityRemaining = (float)((data[8] << 8) | data[9]) / 100.0; 
  Serial.printf("Залишкова ємність [8-9]: %.2f Аг\n", bmsData.capacityRemaining);
  
  bmsData.capacityTotal = (float)((data[10] << 8) | data[11]) / 100.0; 
  Serial.printf("Загальна ємність [10-11]: %.2f Аг\n", bmsData.capacityTotal);
  
  bmsData.cycleCount = (data[12] << 8) | data[13]; 
  Serial.printf("Кількість циклів [12-13]: %d\n", bmsData.cycleCount);
  
  uint16_t prodDate = (data[14] << 8) | data[15];
  uint8_t day = prodDate & 0x1F;
  uint8_t month = (prodDate >> 5) & 0x0F;
  uint16_t year = 2000 + (prodDate >> 9);
  bmsData.productionDate = String(day) + "." + String(month) + "." + String(year);
  Serial.printf("Дата виробництва [14-15]: %s\n", bmsData.productionDate.c_str());
  
  // Balance status [16-19]
  bmsData.balanceStatus = (data[16] << 8) | data[17];      // Байти 16-17
  bmsData.balanceStatusHigh = (data[18] << 8) | data[19];  // Байти 18-19
  Serial.printf("Баланс статус [16-19]: 0x%04X%04X\n", bmsData.balanceStatusHigh, bmsData.balanceStatus);
  
  bmsData.protectionStatus = (data[20] << 8) | data[21]; 
  Serial.printf("Статус захисту [20-21]: 0x%04X\n", bmsData.protectionStatus);
  
  bmsData.softwareVersion = data[22];  // Байт 22 - Версія ПЗ!
  Serial.printf("Версія ПЗ [22]: %d\n", bmsData.softwareVersion);
  
  bmsData.soc = data[23];  // Байт 23 - SOC!
  Serial.printf("SOC [23]: %d %%\n", bmsData.soc);
  
  bmsData.fetStatus = data[24];  // Байт 24 - FET статус!
  Serial.printf("Статус FET [24]: 0x%02X (%s)\n", bmsData.fetStatus, 
                getMOSFETStatusDescription(bmsData.fetStatus).c_str());
  
  bmsData.cellCount = data[25];  // Байт 25 - кількість банок!
  Serial.printf("Кількість банок [25]: %d\n", bmsData.cellCount);
  
  bmsData.tempSensorCount = data[26];  // Байт 26 - кількість датчиків!
  Serial.printf("Кількість датчиків температури [26]: %d\n", bmsData.tempSensorCount);
  
  // Температури починаються з байта 27
  for (int i = 0; i < bmsData.tempSensorCount && i < 6; i++) {
    int offset = 27 + (i * 2);
    uint16_t raw = (data[offset] << 8) | data[offset + 1];
    bmsData.temperatures[i] = (raw - 2731) / 10.0;
    Serial.printf("Температура %d [%d-%d]: %.1f °C (raw: 0x%04X)\n", 
                  i, offset, offset+1, bmsData.temperatures[i], raw);
  }
  
  Serial.println("Дані базової інформації успішно розпарсені");
  return true;
}

bool parseCellVoltages(uint8_t* data, size_t length) {
  if (length < 7 || data[0] != 0xDD || data[1] != 0x04) return false;
  
  uint8_t cellCount = data[3] / 2;
  bmsData.cellCount = cellCount;
  for (int i = 0; i < cellCount && i < 24; i++) {
    uint16_t raw = (data[4 + i*2] << 8) | data[5 + i*2];
    bmsData.cellVoltages[i] = (float)raw / 1000.0;
  }
  return true;
}

bool parseHardwareVersion(uint8_t* data, size_t length) {
  if (length < 7 || data[0] != 0xDD || data[1] != 0x05) return false;
  bmsData.hardwareVersion = "";
  for (int i = 0; i < data[3] && i < 31; i++) {
    bmsData.hardwareVersion += (char)data[4 + i];
  }
  return true;
}

bool readBasicInfo() {
  uint8_t cmd[] = {0xDD, 0xA5, 0x03, 0x00, 0xFF, 0xFD, 0x77};
  return sendBMSCommand(cmd, sizeof(cmd)) && parseBasicInfo(bmsResponse, bmsResponseLength);
}

bool readCellVoltages() {
  uint8_t cmd[] = {0xDD, 0xA5, 0x04, 0x00, 0xFF, 0xFC, 0x77};
  return sendBMSCommand(cmd, sizeof(cmd)) && parseCellVoltages(bmsResponse, bmsResponseLength);
}

bool readHardwareVersion() {
  uint8_t cmd[] = {0xDD, 0xA5, 0x05, 0x00, 0xFF, 0xFB, 0x77};
  return sendBMSCommand(cmd, sizeof(cmd)) && parseHardwareVersion(bmsResponse, bmsResponseLength);
}

bool controlMOSFET(uint8_t state) {
  // Структура: DD 5A E1 02 [DataHigh] [DataLow] [CRC_HI] [CRC_LO] 77
  // DataHigh = 0x00, DataLow = state (00/01/02/03)
  uint8_t cmd[] = {0xDD, 0x5A, 0xE1, 0x02, 0x00, state, 0x00, 0x00, 0x77};
  
  // CRC для запису: FFFF - (Регістр + Довжина + Дані) + 1
  // Дані: E1 + 02 + 00 + state
  uint16_t sum = 0xE1 + 0x02 + 0x00 + state;
  uint16_t checksum = 0xFFFF - sum + 1;
  
  cmd[6] = (checksum >> 8) & 0xFF;  // CRC_HI
  cmd[7] = checksum & 0xFF;         // CRC_LO
  
  Serial.printf("MOSFET cmd: state=0x%02X, sum=0x%04X, CRC=0x%04X\n", state, sum, checksum);
  Serial.print("Command: ");
  for (int i = 0; i < 9; i++) {
    Serial.printf("%02X ", cmd[i]);
  }
  Serial.println();
  
  // Перевірка готових команд
  if (state == 0x03) Serial.println("  -> Увімкнути CHG+DSG (Normal)");
  else if (state == 0x00) Serial.println("  -> Вимкнути все (Emergency)");
  else if (state == 0x01) Serial.println("  -> Увімкнути тільки CHG");
  else if (state == 0x02) Serial.println("  -> Увімкнути тільки DSG");
  
  return sendBMSCommand(cmd, sizeof(cmd));
}

// ============== BLE SCAN & CONNECT ==============
void startBLEScan() {
  // Disconnect from BMS if connected
  if (pClient && bmsConnected) {
    pClient->disconnect();
    bmsConnected = false;
    Serial.println("Disconnected from BMS for scanning");
  }
  
  // Clear saved BMS from memory (but not from display variables yet)
  savedBMSMac = "";
  savedBMSName = "";
  BMS_MAC = "";
  
  // Clear from preferences (optional - remove this if you want to keep for later)
  prefs.begin("hubcfg", false);
  prefs.remove("bmsmac");
  prefs.remove("bmsname");
  prefs.end();
  Serial.println("Cleared saved BMS config");
  
  bmsCount = 0;
  isScanning = true;
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->stop();
  delay(100);
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);
  delay(5000);
  isScanning = false;
}

bool connectToBMS() {
  if (BMS_MAC.length() == 0) return false;
  
  if (pClient) {
    pClient->disconnect();
    delete pClient;
    pClient = nullptr;
  }
  
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallbacks());
  
  if (!pClient->connect(BLEAddress(BMS_MAC.c_str()))) return false;
  
  BLERemoteService* pService = pClient->getService(SERVICE_UUID);
  if (!pService) { pClient->disconnect(); return false; }
  
  pTxCharacteristic = pService->getCharacteristic(CHAR_TX_UUID);
  pRxCharacteristic = pService->getCharacteristic(CHAR_RX_UUID);
  if (!pTxCharacteristic || !pRxCharacteristic) { pClient->disconnect(); return false; }
  
  if (pTxCharacteristic->canNotify()) pTxCharacteristic->registerForNotify(notifyCallback);
  
  bmsConnected = true;
  Serial.println("BMS connected successfully!");
  return true;
}

// ============== ESP-NOW ==============
void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {}

void loadHubConfig() {
  prefs.begin("hubcfg", true);
  String macStr = prefs.getString("mac", "");
  savedBMSMac = prefs.getString("bmsmac", "");
  savedBMSName = prefs.getString("bmsname", "BMS");
  prefs.end();
  
  if (macStr.length() == 17) {
    if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &hub_mac[0], &hub_mac[1], &hub_mac[2],
               &hub_mac[3], &hub_mac[4], &hub_mac[5]) == 6) {
      hub_configured = true;
    }
  }
}

void loadWiFiConfig() {
  prefs.begin("wificfg", true);
  wifi_ssid = prefs.getString("ssid", "");
  wifi_password = prefs.getString("password", "");
  prefs.end();
  
  if (wifi_ssid.length() > 0) {
    wifiConfigured = true;
    Serial.println("[WiFi] Config loaded: " + wifi_ssid);
  }
}

bool connectToWiFi(const char* ssid, const char* password) {
  Serial.printf("[WiFi] Connecting to: %s\n", ssid);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  
  Serial.println("\n[WiFi] Connection failed!");
  return false;
}

void saveHubConfig(String macStr) {
  prefs.begin("hubcfg", false);
  prefs.putString("mac", macStr);
  prefs.end();
  
  if (macStr.length() == 17) {
    uint8_t newMac[6];
    if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &newMac[0], &newMac[1], &newMac[2],
               &newMac[3], &newMac[4], &newMac[5]) == 6) {
      if (hub_configured) esp_now_del_peer(hub_mac);
      memcpy(hub_mac, newMac, 6);
      hub_configured = true;
      
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, hub_mac, 6);
      peerInfo.channel = 1;
      peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
    }
  } else if (macStr.length() == 0) {
    if (hub_configured) esp_now_del_peer(hub_mac);
    memset(hub_mac, 0xFF, 6);
    hub_configured = false;
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, hub_mac, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }
}

void saveBMSConfig(String mac, String name) {
  prefs.begin("hubcfg", false);
  prefs.putString("bmsmac", mac);
  prefs.putString("bmsname", name);
  prefs.end();
  savedBMSMac = mac;
  savedBMSName = name;
}

// Discovery response packet
struct DiscoveryPacket {
  uint8_t magic[4];  // "BMS" + 0x02
  uint8_t mac[6];  // Controller MAC
  bool hasData;    // Has BMS data
  float voltage;   // Last known voltage
  uint8_t soc;     // Last known SOC
};

void onDataRecv(const uint8_t* mac, const uint8_t* incomingData, int len) {
  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  
  Serial.printf("[ESP-NOW] Received from %s, len=%d, data=", macStr, len);
  for (int i = 0; i < len && i < 10; i++) {
    Serial.printf("%02X ", incomingData[i]);
  }
  Serial.println();
  
  // Check if it's a discovery request (4 bytes: 0x42, 0x4D, 0x53, 0x01)
  if (len == 4 && incomingData[0] == 0x42 && incomingData[1] == 0x4D && 
      incomingData[2] == 0x53 && incomingData[3] == 0x01) {
    
    Serial.println("[ESP-NOW] Discovery request received from hub");
    
    // Send discovery response
    DiscoveryPacket response;
    response.magic[0] = 0x42; response.magic[1] = 0x4D;
    response.magic[2] = 0x53; response.magic[3] = 0x02;
    WiFi.macAddress(response.mac);
    response.hasData = bmsConnected;
    response.voltage = bmsData.totalVoltage;
    response.soc = bmsData.soc;
    
    // Add peer for response with current WiFi channel
    // First delete if exists to update channel
    esp_now_del_peer(mac);
    
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = WiFi.channel();  // Use current WiFi channel
    peerInfo.encrypt = false;
    esp_err_t addResult = esp_now_add_peer(&peerInfo);
    
    if (addResult == ESP_OK || addResult == ESP_ERR_ESPNOW_EXIST) {
      delay(10);
      esp_err_t sendResult = esp_now_send(mac, (uint8_t*)&response, sizeof(response));
      Serial.printf("[ESP-NOW] Discovery response sent, result=%d\n", sendResult);
    } else {
      Serial.printf("[ESP-NOW] Failed to add peer, result=%d\n", addResult);
    }
    
    // Auto-save hub MAC
    if (!hub_configured || memcmp(hub_mac, mac, 6) != 0) {
      memcpy(hub_mac, mac, 6);
      hub_configured = true;
      prefs.begin("hubcfg", false);
      prefs.putString("mac", macStr);
      prefs.end();
      Serial.printf("[ESP-NOW] Hub auto-configured: %s\n", macStr);
    }
  }
}

void initESPNow() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, hub_mac, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

void sendDataToHub() {
  if (!hub_configured) return;
  
  // Simple POD packet for ESP-NOW
  struct Packet {
    float totalVoltage, current, capacityRemaining, capacityTotal;
    uint16_t cycleCount;
    uint8_t soc, cellCount;
    float cellVoltages[24];
    float temperatures[6];
    uint16_t protectionStatus;
    uint8_t fetStatus, tempSensorCount;
    uint8_t softwareVersion;
    char productionDate[12];
    char hardwareVersion[32];
    bool connected;
    uint32_t timestamp;
    uint8_t ip[4];  // Controller IP address
  } packet;
  
  packet.totalVoltage = bmsData.totalVoltage;
  packet.current = bmsData.current;
  packet.capacityRemaining = bmsData.capacityRemaining;
  packet.capacityTotal = bmsData.capacityTotal;
  packet.cycleCount = bmsData.cycleCount;
  packet.soc = bmsData.soc;
  packet.cellCount = bmsData.cellCount;
  memcpy(packet.cellVoltages, bmsData.cellVoltages, sizeof(packet.cellVoltages));
  memcpy(packet.temperatures, bmsData.temperatures, sizeof(packet.temperatures));
  packet.protectionStatus = bmsData.protectionStatus;
  packet.fetStatus = bmsData.fetStatus;
  packet.tempSensorCount = bmsData.tempSensorCount;
  packet.softwareVersion = bmsData.softwareVersion;
  // Safe copy of String to char array
  bmsData.productionDate.toCharArray(packet.productionDate, 12);
  bmsData.hardwareVersion.toCharArray(packet.hardwareVersion, 32);
  packet.connected = bmsConnected;
  packet.timestamp = millis();
  
  // Add controller IP
  IPAddress ip = WiFi.softAPIP();
  packet.ip[0] = ip[0];
  packet.ip[1] = ip[1];
  packet.ip[2] = ip[2];
  packet.ip[3] = ip[3];
  
  // Ensure peer exists
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, hub_mac, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;
  esp_err_t addResult = esp_now_add_peer(&peerInfo);
  
  esp_err_t sendResult = esp_now_send(hub_mac, (uint8_t*)&packet, sizeof(packet));
  
  static int sendCount = 0;
  if (++sendCount % 10 == 0) { // Log every 10th send
    Serial.printf("[ESP-NOW] Send result: %d (add peer: %d)\n", sendResult, addResult);
  }
}

// ============== WEB HANDLERS ==============
void handleRoot();
void handleScan();
void handleConnect();
void handleDisconnect();
void handleData();
void handleControl();
void handleHubConfig();

void handleRoot() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>BMS Controller</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <script defer src="https://cdn.jsdelivr.net/npm/alpinejs@3.x.x/dist/cdn.min.js"></script>
    <style>
        [x-cloak] { display: none !important; }
        .cell-bar { transition: width 0.5s ease; }
    </style>
</head>
<body class="bg-gray-900 text-white min-h-screen">
    <div x-data="controllerApp()" x-init="init()" class="p-4 max-w-md mx-auto">
        <!-- Header -->
        <div class="flex justify-between items-center mb-6">
            <div>
                <h1 class="text-xl font-bold text-green-400">BMS Controller</h1>
                <p class="text-xs text-gray-500" x-text="ip || 'Connecting...'"></p>
            </div>
            <span class="px-3 py-1 rounded-full text-xs font-semibold"
                  :class="connected ? 'bg-green-900 text-green-300' : 'bg-red-900 text-red-300'"
                  x-text="connected ? 'Online' : 'Offline'"></span>
        </div>

        <!-- SOC Card -->
        <div class="bg-gray-800 rounded-2xl p-6 mb-4 text-center border border-gray-700 relative overflow-hidden">
            <div class="absolute inset-0 bg-gradient-to-b from-green-500/10 to-transparent"></div>
            <div class="relative">
                <div class="text-6xl font-black mb-2"
                     :class="soc < 20 ? 'text-red-500' : soc < 50 ? 'text-yellow-400' : 'text-green-400'"
                     x-text="soc + '%'"></div>
                <div class="text-gray-400 text-lg" x-text="voltage.toFixed(2) + ' V'"></div>
                <div class="text-sm text-gray-500 mt-1" x-text="current.toFixed(2) + ' A • ' + power.toFixed(1) + ' W'"></div>
            </div>
            <!-- SOC Bar -->
            <div class="mt-4 h-2 bg-gray-700 rounded-full overflow-hidden">
                <div class="h-full rounded-full cell-bar"
                     :class="soc < 20 ? 'bg-red-500' : soc < 50 ? 'bg-yellow-400' : 'bg-green-400'"
                     :style="'width:' + soc + '%'"></div>
            </div>
        </div>

        <!-- Quick Stats -->
        <div class="grid grid-cols-3 gap-2 mb-6">
            <div class="bg-gray-800 p-3 rounded-xl border border-gray-700 text-center">
                <div class="text-xs text-gray-500 mb-1">Temp</div>
                <div class="text-sm font-bold" :class="tempMax > 45 ? 'text-red-400' : 'text-blue-400'" x-text="tempMax + '°C'"></div>
            </div>
            <div class="bg-gray-800 p-3 rounded-xl border border-gray-700 text-center">
                <div class="text-xs text-gray-500 mb-1">Cycles</div>
                <div class="text-sm font-bold text-white" x-text="cycles"></div>
            </div>
            <div class="bg-gray-800 p-3 rounded-xl border border-gray-700 text-center">
                <div class="text-xs text-gray-500 mb-1">Cells</div>
                <div class="text-sm font-bold text-white" x-text="cellCount"></div>
            </div>
        </div>

        <!-- MOSFET Control -->
        <div class="grid grid-cols-2 gap-3 mb-6">
            <button @click="toggleMosfet('charge')" 
                    :class="chargeFet ? 'bg-green-600 hover:bg-green-500' : 'bg-red-600 hover:bg-red-500'"
                    class="p-4 rounded-xl font-bold transition flex items-center justify-center gap-2">
                <span x-text="chargeFet ? '&#128277;' : '&#128274;'"></span>
                <span>Заряд: <span x-text="chargeFet ? 'ON' : 'OFF'"></span></span>
            </button>
            <button @click="toggleMosfet('discharge')"
                    :class="dischargeFet ? 'bg-green-600 hover:bg-green-500' : 'bg-red-600 hover:bg-red-500'"
                    class="p-4 rounded-xl font-bold transition flex items-center justify-center gap-2">
                <span x-text="dischargeFet ? '&#128277;' : '&#128274;'"></span>
                <span>Розряд: <span x-text="dischargeFet ? 'ON' : 'OFF'"></span></span>
            </button>
        </div>

        <!-- Protection Status -->
        <div x-show="protectionStatus !== 'Normal' && protectionStatus !== 'OK'" 
             class="mb-4 p-3 bg-red-900/50 border border-red-700 rounded-xl text-center">
            <span class="text-red-300 font-semibold" x-text="protectionStatus"></span>
        </div>

        <!-- Cells Grid -->
        <h2 class="text-sm text-gray-500 mb-3 uppercase tracking-wider">Комірки (Cells)</h2>
        <div class="grid grid-cols-2 gap-2 mb-6">
            <template x-for="(cell, index) in cells" :key="index">
                <div class="bg-gray-800 p-3 rounded-xl flex justify-between items-center border-l-4"
                     :class="cell.voltage > 4.15 ? 'border-green-500' : cell.voltage > 3.0 ? 'border-yellow-500' : 'border-red-500'">
                    <div class="flex flex-col">
                        <span class="text-gray-500 text-xs" x-text="'#' + (index+1)"></span>
                        <span x-show="cell.balancing" class="text-xs">&#128293;</span>
                    </div>
                    <div class="text-right">
                        <span class="font-mono font-bold" :class="cell.voltage < 3.0 ? 'text-red-400' : 'text-white'" 
                              x-text="cell.voltage.toFixed(3) + 'V'"></span>
                    </div>
                </div>
            </template>
        </div>

        <!-- WiFi Settings -->
        <div class="bg-gray-800 rounded-xl p-4 border border-gray-700 mb-4">
            <h3 class="text-sm font-semibold text-gray-300 mb-3 flex items-center gap-2">
                <span>&#128736;</span> WiFi & Hub
            </h3>
            
            <!-- BLE Scan -->
            <div class="mb-4">
                <button @click="scanBle()" :disabled="scanningBle"
                        class="w-full bg-blue-600 hover:bg-blue-500 disabled:bg-gray-600 p-3 rounded-lg font-semibold transition">
                    <span x-show="!scanningBle">&#128269; Сканувати BLE</span>
                    <span x-show="scanningBle">Сканування...</span>
                </button>
                
                <div x-show="bleDevices.length > 0" class="mt-2 space-y-1">
                    <template x-for="device in bleDevices" :key="device">
                        <div @click="selectBle(device)"
                             class="p-2 bg-gray-700 rounded cursor-pointer hover:bg-gray-600 text-sm"
                             :class="selectedBle === device ? 'ring-2 ring-blue-500' : ''"
                             x-text="device"></div>
                    </template>
                    <button @click="connectBle()" x-show="selectedBle" 
                            class="w-full mt-2 bg-green-600 hover:bg-green-500 p-2 rounded text-sm font-semibold">
                        &#128279; Підключити
                    </button>
                </div>
            </div>

            <!-- Hub MAC -->
            <div class="space-y-2">
                <input type="text" x-model="hubMac" placeholder="Hub MAC (AA:BB:CC:DD:EE:FF)"
                       class="w-full bg-gray-900 border border-gray-600 rounded-lg p-2 text-sm text-white placeholder-gray-500">
                <button @click="saveHubMac()" 
                        class="w-full bg-purple-600 hover:bg-purple-500 p-2 rounded-lg text-sm font-semibold">
                    Зберегти Hub MAC
                </button>
            </div>

            <!-- WiFi Networks -->
            <div class="mt-4 pt-4 border-t border-gray-700">
                <button @click="scanWiFi()" :disabled="scanningWiFi"
                        class="w-full bg-indigo-600 hover:bg-indigo-500 disabled:bg-gray-600 p-2 rounded-lg text-sm font-semibold mb-2">
                    <span x-show="!scanningWiFi">&#128246; Сканувати WiFi</span>
                    <span x-show="scanningWiFi">Сканування...</span>
                </button>
                
                <div x-show="wifiNetworks.length > 0" class="mb-2 max-h-32 overflow-y-auto space-y-1">
                    <template x-for="net in wifiNetworks" :key="net.ssid">
                        <div @click="wifiSsid = net.ssid"
                             class="p-2 bg-gray-700 rounded cursor-pointer hover:bg-gray-600 text-sm flex justify-between"
                             :class="wifiSsid === net.ssid ? 'ring-1 ring-indigo-500' : ''">
                            <span x-text="net.ssid"></span>
                            <span class="text-gray-500" x-text="net.rssi + 'dBm'"></span>
                        </div>
                    </template>
                </div>
                
                <input type="password" x-model="wifiPassword" placeholder="WiFi Password"
                       class="w-full bg-gray-900 border border-gray-600 rounded-lg p-2 text-sm text-white placeholder-gray-500 mb-2">
                <button @click="connectWiFi()"
                        class="w-full bg-green-600 hover:bg-green-500 p-2 rounded-lg text-sm font-semibold">
                    &#128279; Підключити до WiFi
                </button>
            </div>
        </div>

        <!-- Refresh -->
        <button @click="refreshData()" :disabled="loading"
                class="w-full bg-gray-700 hover:bg-gray-600 disabled:bg-gray-800 p-3 rounded-xl font-semibold transition flex items-center justify-center gap-2">
            <span x-show="!loading">&#128260; Оновити дані</span>
            <span x-show="loading">Оновлення...</span>
        </button>

        <!-- Footer -->
        <div class="mt-6 text-center text-xs text-gray-600">
            BMS Controller v1.0 • ESP32
        </div>
    </div>

    <script>
        function controllerApp() {
            return {
                connected: false,
                loading: false,
                ip: '',
                soc: 0,
                voltage: 0,
                current: 0,
                power: 0,
                tempMax: 0,
                cycles: 0,
                cellCount: 0,
                protectionStatus: 'Normal',
                chargeFet: false,
                dischargeFet: false,
                cells: [],
                
                // BLE
                scanningBle: false,
                bleDevices: [],
                selectedBle: '',
                
                // WiFi
                scanningWiFi: false,
                wifiNetworks: [],
                wifiSsid: '',
                wifiPassword: '',
                hubMac: '',

                init() {
                    this.refreshData();
                    setInterval(() => this.refreshData(), 5000);
                },

                async refreshData() {
                    this.loading = true;
                    try {
                        const res = await fetch('/data');
                        const data = await res.json();
                        
                        this.connected = data.connected;
                        this.soc = data.soc || 0;
                        this.voltage = data.voltage || 0;
                        this.current = data.current || 0;
                        this.power = this.voltage * this.current;
                        this.cycles = data.cycles || 0;
                        this.cellCount = data.cellCount || 0;
                        this.protectionStatus = data.protectionStatusDesc || 'Normal';
                        this.chargeFet = data.fetStatus?.includes('charge') || false;
                        this.dischargeFet = data.fetStatus?.includes('discharge') || false;
                        this.tempMax = Math.max(...(data.temps || [0]));
                        this.ip = data.ip || '';
                        
                        // Parse cells with balancing info
                        const balanceStatus = data.balanceStatus || 0;
                        const balanceStatusHigh = data.balanceStatusHigh || 0;
                        this.cells = (data.cells || []).map((v, i) => ({
                            voltage: v,
                            balancing: i < 16 ? (balanceStatus & (1 << i)) !== 0 : (balanceStatusHigh & (1 << (i - 16))) !== 0
                        }));
                    } catch(e) {
                        this.connected = false;
                    }
                    this.loading = false;
                },

                async toggleMosfet(type) {
                    const cmd = type === 'charge' 
                        ? (this.chargeFet ? 'charge_off' : 'charge_on')
                        : (this.dischargeFet ? 'discharge_off' : 'discharge_on');
                    await fetch('/control?cmd=' + cmd);
                    setTimeout(() => this.refreshData(), 500);
                },

                async scanBle() {
                    this.scanningBle = true;
                    try {
                        const res = await fetch('/scan');
                        const data = await res.json();
                        this.bleDevices = data.devices || [];
                    } catch(e) {}
                    this.scanningBle = false;
                },

                selectBle(device) {
                    this.selectedBle = device;
                },

                async connectBle() {
                    if (!this.selectedBle) return;
                    const [mac, name] = this.selectedBle.split(' - ');
                    await fetch('/connect?mac=' + encodeURIComponent(mac) + '&name=' + encodeURIComponent(name || 'BMS'));
                    this.refreshData();
                },

                async saveHubMac() {
                    await fetch('/hubconfig?mac=' + encodeURIComponent(this.hubMac));
                },

                async scanWiFi() {
                    this.scanningWiFi = true;
                    try {
                        const res = await fetch('/api/wifi/scan');
                        const data = await res.json();
                        this.wifiNetworks = data.networks || [];
                    } catch(e) {}
                    this.scanningWiFi = false;
                },

                async connectWiFi() {
                    await fetch('/api/wifi?ssid=' + encodeURIComponent(this.wifiSsid) + 
                              '&pass=' + encodeURIComponent(this.wifiPassword));
                }
            }
        }
    </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleScan() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  startBLEScan();
  DynamicJsonDocument doc(1024);
  doc["connected"] = bmsConnected;
  JsonArray devices = doc.createNestedArray("devices");
  for (int i = 0; i < bmsCount; i++) devices.add(availableBMS[i]);
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleConnect() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String mac = server.arg("mac");
  String name = server.arg("name");
  DynamicJsonDocument doc(512);
  
  if (mac.length() == 17) {
    BMS_MAC = mac;
    BMS_NAME = name;
    savedBMSMac = mac;        // Update for auto-connect
    savedBMSName = name;      // Update for auto-connect
    saveBMSConfig(mac, name);
    if (connectToBMS()) {
      doc["success"] = true;
      doc["connected"] = true;
      doc["message"] = "Connected and saved";
    } else {
      doc["success"] = false;
      doc["connected"] = false;
      doc["message"] = "Connection failed";
    }
  } else {
    doc["success"] = false;
    doc["connected"] = false;
    doc["message"] = "Invalid MAC";
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleDisconnect() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (pClient) { pClient->disconnect(); bmsConnected = false; }
  server.send(200, "application/json", "{\"connected\":false}");
}

void handleData() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  Serial.printf("handleData: bmsConnected=%s, voltage=%.2f\n", 
                bmsConnected ? "true" : "false", bmsData.totalVoltage);
  
  DynamicJsonDocument doc(4096);
  doc["connected"] = bmsConnected;
  
  if (bmsConnected) {
    doc["voltage"] = bmsData.totalVoltage;
    doc["current"] = bmsData.current;
    doc["soc"] = bmsData.soc;
    doc["capacity"] = bmsData.capacityRemaining;
    doc["capacityTotal"] = bmsData.capacityTotal;
    doc["power"] = bmsData.totalVoltage * bmsData.current;
    doc["cycles"] = bmsData.cycleCount;
    doc["cellCount"] = bmsData.cellCount;
    doc["productionDate"] = bmsData.productionDate;
    doc["softwareVersion"] = bmsData.softwareVersion;
    doc["hardwareVersion"] = bmsData.hardwareVersion;
    doc["fetStatus"] = bmsData.fetStatus;
    doc["fetStatusDesc"] = getMOSFETStatusDescription(bmsData.fetStatus);
    doc["protectionStatusDesc"] = getProtectionStatusDescription(bmsData.protectionStatus);
    doc["balanceStatus"] = bmsData.balanceStatus;
    doc["balanceStatusHigh"] = bmsData.balanceStatusHigh;
    
    JsonArray cells = doc.createNestedArray("cells");
    for (int i = 0; i < bmsData.cellCount && i < 24; i++) cells.add(bmsData.cellVoltages[i]);
    
    JsonArray temps = doc.createNestedArray("temperatures");
    for (int i = 0; i < bmsData.tempSensorCount && i < 6; i++) temps.add(bmsData.temperatures[i]);
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleControl() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String cmd = server.arg("cmd");
  String state = server.arg("state");
  String response = "";
  
  if (!bmsConnected) {
    server.send(200, "text/plain", "BMS not connected");
    return;
  }
  
  if (cmd == "basic") response = readBasicInfo() ? "Basic info read" : "Read error";
  else if (cmd == "cells") response = readCellVoltages() ? "Cell voltages read" : "Read error";
  else if (cmd == "hardware") response = readHardwareVersion() ? "Hardware version read" : "Read error";
  else if (cmd == "mos") {
    // MOSFET control with proper bit manipulation
    // state can be: chg_off, chg_on, dsg_off, dsg_on, all_off, all_on
    // OR numeric: 0, 1, 2, 3
    uint8_t currentStatus = bmsData.fetStatus;
    uint8_t newStatus = currentStatus;
    
    if (state == "chg_off") {
      newStatus = currentStatus & ~0x01;  // Clear bit 0 (Charge OFF)
    } else if (state == "chg_on") {
      newStatus = currentStatus | 0x01;   // Set bit 0 (Charge ON)
    } else if (state == "dsg_off") {
      newStatus = currentStatus & ~0x02;  // Clear bit 1 (Discharge OFF)
    } else if (state == "dsg_on") {
      newStatus = currentStatus | 0x02;   // Set bit 1 (Discharge ON)
    } else if (state == "all_off") {
      newStatus = 0x00;  // Both OFF
    } else if (state == "all_on") {
      newStatus = 0x03;  // Both ON
    } else {
      // Fallback to numeric value
      newStatus = state.toInt() & 0x03;
    }
    
    Serial.printf("MOSFET control: current=0x%02X, action=%s, new=0x%02X\n", 
                  currentStatus, state.c_str(), newStatus);
    
    bool result = controlMOSFET(newStatus);
    
    // Read back status to confirm
    if (result) {
      delay(300);
      readBasicInfo();
      response = "MOSFET set to 0x" + String(newStatus, HEX) + 
                 ", now: CHG=" + String((bmsData.fetStatus & 0x01) ? "ON" : "OFF") +
                 ", DSG=" + String((bmsData.fetStatus & 0x02) ? "ON" : "OFF");
    } else {
      response = "MOSFET control error";
    }
  }
  else response = "Unknown command";
  
  server.send(200, "text/plain", response);
}

void handleHubConfig() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String macStr = server.arg("mac");
  DynamicJsonDocument doc(256);
  
  if (macStr.length() == 0) {
    hub_configured = false;
    memset(hub_mac, 0xFF, 6);
    doc["success"] = true;
    doc["message"] = "Broadcast mode enabled";
  } else if (macStr.length() == 17) {
    saveHubConfig(macStr);
    doc["success"] = true;
    doc["message"] = "Hub MAC saved: " + macStr;
  } else {
    doc["success"] = false;
    doc["message"] = "Invalid MAC format. Use AA:BB:CC:DD:EE:FF";
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleWiFiScan() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  int n = WiFi.scanNetworks();
  DynamicJsonDocument doc(2048);
  JsonArray networks = doc.createNestedArray("networks");
  
  for (int i = 0; i < n; i++) {
    JsonObject net = networks.createNestedObject();
    net["ssid"] = WiFi.SSID(i);
    net["rssi"] = WiFi.RSSI(i);
    net["encrypted"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleWiFiConfig() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String newSsid = server.arg("ssid");
    String newPass = server.arg("password");
    
    prefs.begin("wificfg", false);
    prefs.putString("ssid", newSsid);
    prefs.putString("password", newPass);
    prefs.end();
    
    wifi_ssid = newSsid;
    wifi_password = newPass;
    wifiConfigured = true;
    
    // Try to connect
    connectToWiFi(newSsid.c_str(), newPass.c_str());
    
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing parameters\"}");
  }
}

// ============== SETUP & LOOP ==============
void controllerSetup() {
  Serial.begin(115200);
  delay(1000);
  
  memset(&bmsData, 0, sizeof(bmsData));
  
  // Init display
  initControllerDisplay();
  
  // Load WiFi config first
  loadWiFiConfig();
  
  // Init WiFi - STA first, then AP if needed
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  
  // Try to connect to WiFi if configured
  if (wifiConfigured) {
    if (connectToWiFi(wifi_ssid.c_str(), wifi_password.c_str())) {
      Serial.println("[WiFi] Connected to network, starting AP for config");
    }
  }
  
  // Always enable AP for local access (needed for web config)
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(ap_ssid, ap_password, ESPNOW_CHANNEL, false, 4);
  
  // Wait for WiFi connection and print current channel
  if (wifiConfigured) {
    Serial.printf("[WiFi] Current channel after connection: %d\n", WiFi.channel());
  }
  
  // Init BLE
  BLEDevice::init("BMS-Controller");
  
  // Init ESP-NOW AFTER WiFi is fully configured (to get correct channel)
  initESPNow();
  
  // Print MAC address for debugging
  uint8_t mac[6];
  WiFi.macAddress(mac);
  Serial.printf("[WiFi] Controller MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("[WiFi] AP IP: %s  STA IP: %s\n", 
                WiFi.softAPIP().toString().c_str(),
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "not connected");
  
  // Web server routes
  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/connect", handleConnect);
  server.on("/disconnect", handleDisconnect);
  server.on("/data", handleData);
  server.on("/control", handleControl);
  server.on("/hubconfig", handleHubConfig);
  server.on("/api/wifi/scan", handleWiFiScan);
  server.on("/api/wifi", handleWiFiConfig);
  
  // CORS preflight for all routes
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
      server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
      server.send(200);
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });
  
  server.begin();
  
  // Load saved hub config
  loadHubConfig();
  
  Serial.println("Controller ready");
}

void controllerLoop() {
  server.handleClient();
  
  // Auto-connect to saved BMS
  static unsigned long lastAutoConnect = 0;
  if (!bmsConnected && savedBMSMac.length() == 17 && millis() - lastAutoConnect > 5000) {
    lastAutoConnect = millis();
    BMS_MAC = savedBMSMac;
    Serial.println("Auto-connecting to saved BMS: " + BMS_MAC);
    if (connectToBMS()) {
      Serial.println("Auto-connect successful!");
      // Read data immediately after connection
      delay(200);
      if (readBasicInfo()) {
        Serial.println("Initial data read after auto-connect");
        readCellVoltages();
      }
    } else {
      Serial.println("Auto-connect failed");
    }
  }
  
  // Read BMS data periodically
  if (bmsConnected && millis() - lastBMSRead > 2000) {
    lastBMSRead = millis();
    Serial.println("Reading BMS data...");
    if (readBasicInfo()) {
      Serial.println("Basic info read OK");
      readCellVoltages();
    } else {
      Serial.println("Basic info read FAILED");
    }
    Serial.printf("Voltage: %.2f, Current: %.2f, SOC: %d\n", bmsData.totalVoltage, bmsData.current, bmsData.soc);
  }
  
  // Send to hub via ESP-NOW
  if (bmsConnected && millis() - lastDataSend > 1000) {
    lastDataSend = millis();
    sendDataToHub();
  }
  
  // Update display
  if (millis() - lastDisplayUpdate > 1000) {
    lastDisplayUpdate = millis();
    updateControllerDisplay(bmsData, bmsConnected, hub_configured);
  }
  
  delay(50);
}

#endif // CONTROLLER_H
