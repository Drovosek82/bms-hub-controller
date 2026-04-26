// hub.h - BMS Hub code (ESP-NOW receiver + Web Server)
// This file is included in main.cpp when MODE_HUB is defined

#ifndef HUB_H
#define HUB_H

#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "display_hub.h"

// ============== CONFIGURATION ==============
LGFX_MGT020 tft;
WebServer server(80);
Preferences prefs;

// WiFi AP configuration
const char* hub_ssid = "BMS_HUB";
const char* hub_password = "12345678";

// WiFi STA configuration (loaded from preferences)
String wifi_ssid = "";
String wifi_password = "";
bool wifiConfigured = false;

// Tunnel configuration
// bool tunnelActive = false;
// String tunnelUrl = "";

// ESP-NOW channel (must match on all devices)
#define ESPNOW_CHANNEL 1

// ============== DATA STRUCTURES ==============
// BMSData and ControllerData defined in display_hub.h

// Registration structure for IP
struct RegistrationData {
  uint32_t magic;
  uint8_t ip[4];
  uint16_t port;
};
#define REG_MAGIC 0x424D5352

#define MAX_CONTROLLERS 20
ControllerData controllers[MAX_CONTROLLERS];
int activeControllerCount = 0;

unsigned long lastDisplayUpdate = 0;
unsigned long lastAggregation = 0;

// ============== CONTROLLER PERSISTENCE ==============
void saveControllers() {
  prefs.begin("controllers", false);
  int count = 0;
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (controllers[i].active) {
      char key[16];
      sprintf(key, "ctrl%d", count);
      prefs.putBytes(key, controllers[i].mac, 6);
      count++;
    }
  }
  prefs.putInt("count", count);
  prefs.end();
  Serial.printf("[HUB] Saved %d controllers\n", count);
}

void loadControllers() {
  prefs.begin("controllers", true);
  int count = prefs.getInt("count", 0);
  for (int i = 0; i < count && i < MAX_CONTROLLERS; i++) {
    char key[16];
    sprintf(key, "ctrl%d", i);
    if (prefs.getBytes(key, controllers[i].mac, 6) == 6) {
      controllers[i].active = true;
      controllers[i].lastSeen = 0; // Will be updated when data arrives
      Serial.printf("[HUB] Loaded controller %d: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    i, controllers[i].mac[0], controllers[i].mac[1],
                    controllers[i].mac[2], controllers[i].mac[3],
                    controllers[i].mac[4], controllers[i].mac[5]);
    }
  }
  prefs.end();
}

// Discovery response packet (must match controller.h)
struct DiscoveryPacket {
  uint8_t magic[4];  // "BMS" + 0x02
  uint8_t mac[6];  // Controller MAC
  bool hasData;    // Has BMS data
  float voltage;   // Last known voltage
  uint8_t soc;     // Last known SOC
};

// ============== ESP-NOW CALLBACKS ==============
void onDataRecv(const uint8_t* mac, const uint8_t* incomingData, int len) {
  Serial.print("ESP-NOW data from: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", mac[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.print(" len=");
  Serial.println(len);
  
  // Check if it's discovery response (0x42, 0x4D, 0x53, 0x02)
  if (len == sizeof(DiscoveryPacket)) {
    DiscoveryPacket* disc = (DiscoveryPacket*)incomingData;
    if (disc->magic[0] == 0x42 && disc->magic[1] == 0x4D && 
        disc->magic[2] == 0x53 && disc->magic[3] == 0x02) {
      Serial.printf("Discovery response from %02X:%02X:%02X:%02X:%02X:%02X V=%.2f SOC=%d%%\n",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                    disc->voltage, disc->soc);
      
      // Find or create controller slot
      int slot = -1;
      for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (controllers[i].active && 
            memcmp(controllers[i].mac, mac, 6) == 0) {
          slot = i;
          break;
        }
        if (!controllers[i].active && slot == -1) {
          slot = i;
        }
      }
      
      if (slot >= 0) {
        memcpy(controllers[slot].mac, mac, 6);
        controllers[slot].data.totalVoltage = disc->voltage;
        controllers[slot].data.soc = disc->soc;
        controllers[slot].data.connected = disc->hasData;
        controllers[slot].lastSeen = millis();
        controllers[slot].active = true;
        Serial.printf("Controller %d registered from discovery\n", slot);
        saveControllers(); // Save to persistent storage
      }
      return;
    }
  }
  
  // Check if it's IP registration
  if (len == sizeof(RegistrationData)) {
    RegistrationData* reg = (RegistrationData*)incomingData;
    if (reg->magic == REG_MAGIC) {
      Serial.printf("Registration from %02X:%02X:%02X:%02X:%02X:%02X\n",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      return;
    }
  }
  
  // Must be BMS data
  if (len != sizeof(BMSData)) {
    Serial.printf("ERROR: Invalid data size. Expected %d, got %d\n", 
                  sizeof(BMSData), len);
    return;
  }
  
  BMSData* data = (BMSData*)incomingData;
  
  // Find or create controller slot
  int slot = -1;
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (controllers[i].active && 
        memcmp(controllers[i].mac, mac, 6) == 0) {
      slot = i;
      break;
    }
    if (!controllers[i].active && slot == -1) {
      slot = i;
    }
  }
  
  if (slot >= 0) {
    memcpy(controllers[slot].mac, mac, 6);
    memcpy(&controllers[slot].data, data, sizeof(BMSData));
    controllers[slot].lastSeen = millis();
    controllers[slot].active = true;
    
    Serial.printf("Controller %d: V=%.2fV I=%.1fA SOC=%d%% Cells=%d\n",
                  slot, data->totalVoltage, data->current, 
                  data->soc, data->cellCount);
  }
}

void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
  // Hub rarely sends data, but callback needed
}

// ============== WIFI FUNCTIONS ==============
bool connectToWiFi(const char* ssid, const char* password) {
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
    // Disable AP since we have WiFi connection
    WiFi.softAPdisconnect(true);
    Serial.println("[WiFi] AP mode disabled, using STA only");
    return true;
  }
  
  Serial.println("\nWiFi connection failed! Starting AP...");
  // Enable AP for configuration
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(hub_ssid, hub_password, ESPNOW_CHANNEL, false, 4);
  Serial.printf("AP started: %s\n", hub_ssid);
  return false;
}

// ============== DISPLAY ==============
// Display functions moved to display_hub.h
// Using: initHubDisplay() and updateHubDisplay()

void updateDisplay() {
  if (millis() - lastDisplayUpdate < 1000) return;
  lastDisplayUpdate = millis();
  
  // Count active controllers
  activeControllerCount = 0;
  float totalVoltage = 0;
  float totalCurrent = 0;
  int totalSOC = 0;
  
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (controllers[i].active && 
        millis() - controllers[i].lastSeen < 10000) {
      activeControllerCount++;
      totalVoltage += controllers[i].data.totalVoltage;
      totalCurrent += controllers[i].data.current;
      totalSOC += controllers[i].data.soc;
    } else {
      controllers[i].active = false;
    }
  }
  
  float power = totalVoltage * totalCurrent;
  int avgSOC = activeControllerCount > 0 ? totalSOC / activeControllerCount : 0;
  
  // Serial output
  Serial.printf("[HUB] Active: %d/%d, V=%.2fV, I=%.1fA, P=%.0fW, SOC=%d%%\n",
                activeControllerCount, MAX_CONTROLLERS,
                totalVoltage, totalCurrent, power, avgSOC);
  
  // Call display function from display_hub.h
  updateHubDisplay(activeControllerCount, MAX_CONTROLLERS,
                   totalVoltage, totalCurrent, avgSOC,
                   controllers, MAX_CONTROLLERS);
  
  // WiFi status overlay
  tft.setTextDatum(top_right);
  tft.setFont(&fonts::Font0);
  
  if (WiFi.status() == WL_CONNECTED) {
    // Show RSSI and IP when connected
    int rssi = WiFi.RSSI();
    int rssiColor = (rssi > -50) ? TFT_GREEN : (rssi > -70) ? TFT_YELLOW : TFT_RED;
    
    char wifiBuf[32];
    sprintf(wifiBuf, "%ddBm", rssi);
    tft.setTextColor(rssiColor);
    tft.drawString(wifiBuf, 315, 5);
    
    tft.setTextDatum(top_left);
    tft.setTextColor(TFT_CYAN);
    tft.drawString(WiFi.localIP().toString().c_str(), 5, 5);
  } else {
    // Show AP mode indicator
    tft.setTextColor(TFT_ORANGE);
    tft.drawString("AP", 315, 5);
    
    tft.setTextDatum(top_left);
    tft.setTextColor(TFT_CYAN);
    tft.drawString(WiFi.softAPIP().toString().c_str(), 5, 5);
  }
}

// ============== WEB SERVER ==============

const char HUB_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>BMS Hub Pro</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <script defer src="https://cdn.jsdelivr.net/npm/alpinejs@3.x.x/dist/cdn.min.js"></script>
    <style>
        [x-cloak] { display: none !important; }
        .soc-bar { transition: width 0.5s ease; }
        .cell-bar { transition: width 0.5s ease; }
        .fade-in { animation: fadeIn 0.3s ease-in; }
        @keyframes fadeIn { from { opacity: 0; transform: translateY(10px); } to { opacity: 1; transform: translateY(0); } }
    </style>
</head>
<body class="bg-slate-950 text-slate-100 min-h-screen p-4 font-sans">
    <div x-data="hubApp()" x-init="init()" class="max-w-lg mx-auto">
        <!-- Header -->
        <header class="mb-6">
            <div class="flex justify-between items-center">
                <div>
                    <h1 class="text-2xl font-bold text-blue-400">BMS HUB Pro</h1>
                    <p class="text-slate-500 text-xs" x-text="hubIP || 'Connecting...'"></p>
                </div>
                <div class="flex flex-col items-end">
                    <span class="px-3 py-1 rounded-full text-xs font-semibold mb-1"
                          :class="connected ? 'bg-green-900 text-green-300' : 'bg-red-900 text-red-300'"
                          x-text="connected ? 'Online' : 'Offline'"></span>
                    <span class="text-xs text-slate-600" x-text="'Ch: ' + espNowChannel"></span>
                </div>
            </div>
            <p class="text-slate-600 text-sm mt-1">Агрегація даних з контролерів</p>
        </header>

        <!-- System Overview Cards -->
        <div class="grid grid-cols-2 gap-3 mb-4">
            <div class="bg-slate-900 p-4 rounded-xl border border-slate-800">
                <div class="text-xs text-slate-500 mb-1">Загальна напруга</div>
                <div class="text-2xl font-bold text-blue-400" x-text="totalVoltage.toFixed(1) + 'V'"></div>
            </div>
            <div class="bg-slate-900 p-4 rounded-xl border border-slate-800">
                <div class="text-xs text-slate-500 mb-1">Загальний струм</div>
                <div class="text-2xl font-bold" :class="totalCurrent > 0 ? 'text-green-400' : 'text-red-400'" 
                     x-text="(totalCurrent > 0 ? '+' : '') + totalCurrent.toFixed(1) + 'A'"></div>
            </div>
        </div>

        <!-- Power & SOC Cards -->
        <div class="grid grid-cols-2 gap-3 mb-4">
            <div class="bg-slate-900 p-4 rounded-xl border border-slate-800 text-center">
                <div class="text-xs text-slate-500 mb-2">Потужність</div>
                <div class="text-3xl font-black" :class="power > 0 ? 'text-green-400' : 'text-red-400'" 
                     x-text="power.toFixed(0) + 'W'"></div>
            </div>
            <div class="bg-slate-900 p-4 rounded-xl border border-slate-800 text-center">
                <div class="text-xs text-slate-500 mb-2">Середній SOC</div>
                <div class="text-3xl font-black" 
                     :class="avgSOC > 50 ? 'text-green-400' : avgSOC > 20 ? 'text-yellow-400' : 'text-red-400'" 
                     x-text="avgSOC + '%'"></div>
                <div class="mt-2 h-2 bg-slate-800 rounded-full overflow-hidden">
                    <div class="h-full rounded-full soc-bar"
                         :class="avgSOC > 50 ? 'bg-green-500' : avgSOC > 20 ? 'bg-yellow-500' : 'bg-red-500'"
                         :style="'width:' + avgSOC + '%'"></div>
                </div>
            </div>
        </div>

        <!-- Stats Row -->
        <div class="grid grid-cols-4 gap-2 mb-6">
            <div class="bg-slate-900 p-3 rounded-xl border border-slate-800 text-center">
                <div class="text-xs text-slate-500">Активні</div>
                <div class="text-lg font-bold" x-text="activeControllers + '/' + maxControllers"></div>
            </div>
            <div class="bg-slate-900 p-3 rounded-xl border border-slate-800 text-center">
                <div class="text-xs text-slate-500">Комірок</div>
                <div class="text-lg font-bold" x-text="totalCells"></div>
            </div>
            <div class="bg-slate-900 p-3 rounded-xl border border-slate-800 text-center">
                <div class="text-xs text-slate-500">Temp Max</div>
                <div class="text-lg font-bold" :class="maxTemp > 45 ? 'text-red-400' : 'text-blue-400'" x-text="maxTemp + '°C'"></div>
            </div>
            <div class="bg-slate-900 p-3 rounded-xl border border-slate-800 text-center">
                <div class="text-xs text-slate-500">ESP-NOW</div>
                <div class="text-lg font-bold" :class="espNowPeers > 0 ? 'text-green-400' : 'text-slate-500'" x-text="espNowPeers"></div>
            </div>
        </div>

        <!-- Scan Button -->
        <div class="flex gap-2 mb-6">
            <button @click="scanControllers()" :disabled="scanning"
                    class="flex-1 bg-purple-600 hover:bg-purple-500 disabled:bg-slate-700 p-3 rounded-xl font-semibold transition flex items-center justify-center gap-2">
                <span x-show="!scanning">&#128269; Пошук контролерів</span>
                <span x-show="scanning">&#128260; Пошук...</span>
            </button>
            <button @click="showDebug = !showDebug"
                    class="bg-slate-700 hover:bg-slate-600 p-3 rounded-xl font-semibold transition">
                &#128736;
            </button>
        </div>

        <!-- Debug Panel -->
        <div x-show="showDebug" x-cloak class="mb-6 bg-slate-900 border border-slate-700 rounded-xl p-4 fade-in">
            <h3 class="text-sm font-semibold text-slate-300 mb-3">&#128736; Діагностика ESP-NOW</h3>
            <div class="space-y-2 text-xs">
                <div class="flex justify-between">
                    <span class="text-slate-500">WiFi Канал:</span>
                    <span x-text="espNowChannel" class="font-mono"></span>
                </div>
                <div class="flex justify-between">
                    <span class="text-slate-500">ESP-NOW Peers:</span>
                    <span x-text="espNowPeers" class="font-mono"></span>
                </div>
                <div class="flex justify-between">
                    <span class="text-slate-500">Останнє оновлення:</span>
                    <span x-text="lastUpdate" class="font-mono"></span>
                </div>
                <div class="mt-3 pt-3 border-t border-slate-700">
                    <p class="text-slate-400 mb-2">Всі слоти контролерів:</p>
                    <div class="grid grid-cols-5 gap-1">
                        <template x-for="(slot, idx) in debugSlots" :key="idx">
                            <div class="text-center p-1 rounded text-xs"
                                 :class="slot.active ? 'bg-green-900 text-green-300' : 'bg-slate-800 text-slate-500'"
                                 x-text="idx">
                            </div>
                        </template>
                    </div>
                </div>
            </div>
        </div>

        <!-- Controllers List -->
        <h2 class="mb-4 font-semibold text-slate-400 text-sm uppercase tracking-wider flex items-center gap-2">
            <span>Підключені контролери</span>
            <span x-show="activeControllers > 0" class="text-xs bg-blue-900 text-blue-300 px-2 py-0.5 rounded-full" x-text="activeControllers"></span>
        </h2>
        
        <div class="space-y-4">
            <template x-for="ctrl in controllers" :key="ctrl.id">
                <div class="bg-slate-900 border border-slate-800 rounded-xl overflow-hidden fade-in"
                     :class="selectedController === ctrl.id ? 'ring-2 ring-blue-500' : ''">
                    <!-- Controller Header -->
                    <div @click="selectedController = selectedController === ctrl.id ? -1 : ctrl.id"
                         class="p-4 cursor-pointer hover:bg-slate-800/50 transition">
                        <div class="flex justify-between items-center mb-2">
                            <div class="flex items-center gap-3">
                                <div class="w-3 h-3 rounded-full" 
                                     :class="ctrl.soc > 50 ? 'bg-green-500' : ctrl.soc > 20 ? 'bg-yellow-500' : 'bg-red-500'"></div>
                                <span class="font-bold" x-text="'Controller #' + (ctrl.id + 1)"></span>
                                <span class="text-xs text-slate-500 font-mono" x-text="ctrl.mac"></span>
                            </div>
                            <div class="flex items-center gap-2">
                                <span class="text-xs px-2 py-1 rounded bg-slate-800" 
                                      :class="ctrl.lastSeen < 5 ? 'text-green-400' : 'text-yellow-400'"
                                      x-text="ctrl.lastSeen + 's'"></span>
                                <span class="text-lg font-bold" x-text="ctrl.soc + '%'"></span>
                            </div>
                        </div>
                        
                        <!-- Progress Bar -->
                        <div class="flex items-center gap-3">
                            <div class="flex-1 h-3 bg-slate-800 rounded-full overflow-hidden">
                                <div class="h-full rounded-full soc-bar"
                                     :class="ctrl.soc > 50 ? 'bg-green-500' : ctrl.soc > 20 ? 'bg-yellow-500' : 'bg-red-500'"
                                     :style="'width:' + ctrl.soc + '%'"></div>
                            </div>
                        </div>
                        
                        <!-- Quick Info -->
                        <div class="flex justify-between mt-2 text-xs text-slate-500">
                            <span x-text="ctrl.voltage.toFixed(2) + 'V'"></span>
                            <span x-text="ctrl.current.toFixed(2) + 'A'"></span>
                            <span x-text="ctrl.cellCount + ' cells'"></span>
                            <span x-text="ctrl.tempMax + '°C'"></span>
                        </div>
                    </div>
                    
                    <!-- Expanded Details -->
                    <div x-show="selectedController === ctrl.id" x-cloak class="border-t border-slate-800 p-4 bg-slate-900/50">
                        <!-- MOSFET Status -->
                        <div class="flex gap-2 mb-4">
                            <div class="flex-1 p-2 rounded-lg text-center text-sm"
                                 :class="ctrl.fetStatus & 1 ? 'bg-green-900/50 text-green-300' : 'bg-red-900/50 text-red-300'">
                                <span x-text="ctrl.fetStatus & 1 ? '&#128277; Charge ON' : '&#128274; Charge OFF'"></span>
                            </div>
                            <div class="flex-1 p-2 rounded-lg text-center text-sm"
                                 :class="ctrl.fetStatus & 2 ? 'bg-green-900/50 text-green-300' : 'bg-red-900/50 text-red-300'">
                                <span x-text="ctrl.fetStatus & 2 ? '&#128277; Discharge ON' : '&#128274; Discharge OFF'"></span>
                            </div>
                        </div>
                        
                        <!-- Cell Voltages -->
                        <h4 class="text-xs text-slate-500 mb-2 uppercase">Напруга комірок</h4>
                        <div class="grid grid-cols-4 gap-2 mb-4">
                            <template x-for="(cellV, cidx) in ctrl.cells" :key="cidx">
                                <div class="bg-slate-800 p-2 rounded-lg text-center">
                                    <div class="text-xs text-slate-500" x-text="'#' + (cidx + 1)"></div>
                                    <div class="text-sm font-mono font-bold"
                                         :class="cellV > 4.15 ? 'text-green-400' : cellV < 3.0 ? 'text-red-400' : 'text-white'"
                                         x-text="cellV.toFixed(3) + 'V'"></div>
                                </div>
                            </template>
                        </div>
                        
                        <!-- Protection Status -->
                        <div x-show="ctrl.protectionStatus !== 0" class="p-2 bg-red-900/30 border border-red-800 rounded-lg">
                            <span class="text-red-300 text-sm">&#9888; Захист: <span x-text="getProtectionText(ctrl.protectionStatus)"></span></span>
                        </div>
                        
                        <div class="mt-2 text-xs text-slate-600 text-right" x-text="'IP: ' + ctrl.ip"></div>
                    </div>
                </div>
            </template>
        </div>

        <!-- Empty State -->
        <div x-show="controllers.length === 0 && !scanning" class="text-center py-12 text-slate-600">
            <div class="text-5xl mb-4">&#128268;</div>
            <p class="text-lg mb-2">Немає підключених контролерів</p>
            <p class="text-sm text-slate-500 mb-4">Перевірте що контролери увімкнені та на одному WiFi каналі</p>
            <button @click="scanControllers()" :disabled="scanning"
                    class="bg-purple-600 hover:bg-purple-500 disabled:bg-slate-700 px-6 py-3 rounded-xl font-semibold transition">
                <span x-show="!scanning">&#128269; Сканувати мережу</span>
                <span x-show="scanning">Сканування...</span>
            </button>
        </div>

        <!-- Help -->
        <div class="mt-8 p-4 bg-slate-900/50 border border-slate-800 rounded-xl">
            <h3 class="text-sm font-semibold text-slate-300 mb-2">&#128161; Підказка</h3>
            <p class="text-xs text-slate-500">Якщо контролер видно на дисплеї але не в вебі - перевірте WiFi канал. Хаб і контролер повинні бути на одному каналі.</p>
        </div>

        <!-- Footer -->
        <div class="mt-6 text-center text-xs text-slate-700">
            BMS Hub Pro v2.0 • ESP32 • <span x-text="maxControllers + ' max'"></span>
        </div>
    </div>

    <script>
        function hubApp() {
            return {
                connected: false,
                activeControllers: 0,
                maxControllers: 0,
                totalVoltage: 0,
                totalCurrent: 0,
                power: 0,
                avgSOC: 0,
                totalCells: 0,
                maxTemp: 0,
                espNowChannel: 0,
                espNowPeers: 0,
                hubIP: '',
                lastUpdate: 'never',
                scanning: false,
                showDebug: false,
                selectedController: -1,
                controllers: [],
                debugSlots: [],
                
                init() {
                    this.refreshData();
                    this.loadDebug();
                    setInterval(() => { this.refreshData(); this.loadDebug(); }, 3000);
                },
                
                async refreshData() {
                    try {
                        const res = await fetch('/api/data');
                        const data = await res.json();
                        
                        this.connected = data.connected;
                        this.activeControllers = data.activeControllers;
                        this.maxControllers = data.maxControllers;
                        this.totalVoltage = data.totalVoltage;
                        this.totalCurrent = data.totalCurrent;
                        this.power = data.power;
                        this.avgSOC = Math.round(data.avgSOC);
                        this.espNowChannel = data.espNowChannel || 0;
                        this.espNowPeers = data.espNowPeers || 0;
                        this.hubIP = data.hubIP;
                        this.lastUpdate = new Date().toLocaleTimeString();
                        
                        // Calculate totals
                        this.totalCells = data.controllers?.reduce((sum, c) => sum + (c.cellCount || 0), 0) || 0;
                        this.maxTemp = Math.max(...(data.controllers?.map(c => c.tempMax || 0) || [0]));
                        
                        // Process controllers with cells
                        this.controllers = (data.controllers || []).map(c => ({
                            ...c,
                            cells: c.cells || []
                        }));
                    } catch(e) {
                        this.connected = false;
                        console.error('Refresh error:', e);
                    }
                },
                
                async loadDebug() {
                    try {
                        const res = await fetch('/api/debug');
                        const data = await res.json();
                        this.debugSlots = data.allControllers || [];
                    } catch(e) {}
                },
                
                async scanControllers() {
                    this.scanning = true;
                    try {
                        await fetch('/api/controllers/scan', { method: 'POST' });
                        await new Promise(r => setTimeout(r, 3000));
                        await this.refreshData();
                        await this.loadDebug();
                    } catch(e) {
                        console.error('Scan error:', e);
                    }
                    this.scanning = false;
                },
                
                getProtectionText(status) {
                    const protections = {
                        0: 'OK', 1: 'Cell Overvolt', 2: 'Cell Undervolt', 4: 'Pack Overvolt',
                        8: 'Pack Undervolt', 16: 'Chg Temp', 32: 'Dsg Temp', 64: 'Chg OCP',
                        128: 'Dsg OCP', 256: 'Short', 512: 'IC Error'
                    };
                    let result = [];
                    for (let [bit, name] of Object.entries(protections)) {
                        if (status & parseInt(bit)) result.push(name);
                    }
                    return result.join(', ') || 'OK';
                }
            }
        }
    </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/html", HUB_HTML);
}

// API endpoint for data
void handleApiData() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  
  // Count active controllers
  activeControllerCount = 0;
  float totalVoltage = 0;
  float totalCurrent = 0;
  int totalSOC = 0;
  
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (controllers[i].active && 
        millis() - controllers[i].lastSeen < 10000) {
      activeControllerCount++;
      totalVoltage += controllers[i].data.totalVoltage;
      totalCurrent += controllers[i].data.current;
      totalSOC += controllers[i].data.soc;
    } else {
      controllers[i].active = false;
    }
  }
  
  DynamicJsonDocument doc(4096);
  bool hasData = activeControllerCount > 0;
  doc["connected"] = hasData;
  doc["activeControllers"] = activeControllerCount;
  doc["maxControllers"] = MAX_CONTROLLERS;
  doc["totalVoltage"] = totalVoltage;
  doc["totalCurrent"] = totalCurrent;
  doc["power"] = totalVoltage * totalCurrent;
  doc["avgSOC"] = hasData ? totalSOC / activeControllerCount : 0;
  doc["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
  doc["apIP"] = WiFi.softAPIP().toString();
  doc["staIP"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  doc["hubIP"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  
  // Додати статус ESP-NOW
  doc["espNowChannel"] = WiFi.channel();
  doc["espNowPeers"] = activeControllerCount;
  
  JsonArray controllersArray = doc.createNestedArray("controllers");
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (controllers[i].active) {
      JsonObject ctrl = controllersArray.createNestedObject();
      ctrl["id"] = i;
      char macStr[18];
      sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
              controllers[i].mac[0], controllers[i].mac[1], controllers[i].mac[2],
              controllers[i].mac[3], controllers[i].mac[4], controllers[i].mac[5]);
      ctrl["mac"] = macStr;
      ctrl["voltage"] = controllers[i].data.totalVoltage;
      ctrl["current"] = controllers[i].data.current;
      ctrl["soc"] = controllers[i].data.soc;
      ctrl["cellCount"] = controllers[i].data.cellCount;
      // Обчислити max/min температури
      float tMax = -999, tMin = 999;
      for (int t = 0; t < controllers[i].data.tempSensorCount && t < 6; t++) {
        float temp = controllers[i].data.temperatures[t];
        if (temp > tMax) tMax = temp;
        if (temp < tMin) tMin = temp;
      }
      ctrl["tempMax"] = (tMax > -900) ? tMax : 0;
      ctrl["tempMin"] = (tMin < 900) ? tMin : 0;
      ctrl["fetStatus"] = controllers[i].data.fetStatus;
      ctrl["protectionStatus"] = controllers[i].data.protectionStatus;
      ctrl["lastSeen"] = (millis() - controllers[i].lastSeen) / 1000;
      char ipStr[16];
      sprintf(ipStr, "%d.%d.%d.%d",
              controllers[i].data.ip[0], controllers[i].data.ip[1],
              controllers[i].data.ip[2], controllers[i].data.ip[3]);
      ctrl["ip"] = ipStr;
      
      // Додати масив комірок
      JsonArray cellsArray = ctrl.createNestedArray("cells");
      for (int c = 0; c < controllers[i].data.cellCount && c < 32; c++) {
        cellsArray.add(controllers[i].data.cellVoltages[c] / 1000.0);
      }
    }
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// Scan for controllers - broadcast ESP-NOW discovery
void handleControllerScan() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  
  Serial.println("[SCAN] Starting controller scan...");
  
  // Get current WiFi channel
  int currentChannel = WiFi.channel();
  if (currentChannel == 0) currentChannel = ESPNOW_CHANNEL;
  Serial.printf("[SCAN] Current WiFi channel: %d\n", currentChannel);
  
  // Ensure broadcast peer exists with correct channel
  uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  
  // Remove existing peer if channel changed
  if (esp_now_is_peer_exist(broadcastAddr)) {
    esp_now_del_peer(broadcastAddr);
  }
  
  // Add peer with current channel
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddr, 6);
  peerInfo.channel = currentChannel;
  peerInfo.encrypt = false;
  esp_err_t addResult = esp_now_add_peer(&peerInfo);
  Serial.printf("[SCAN] Added broadcast peer on channel %d: %d\n", currentChannel, addResult);
  
  // Send broadcast discovery packet via ESP-NOW
  uint8_t discoveryData[4] = {0x42, 0x4D, 0x53, 0x01}; // "BMS" + version
  
  esp_err_t result = esp_now_send(broadcastAddr, discoveryData, 4);
  
  Serial.printf("[SCAN] Broadcast sent, result=%d (%s)\n", result, 
                result == ESP_OK ? "OK" : "FAIL");
  
  DynamicJsonDocument doc(256);
  doc["success"] = (result == ESP_OK);
  doc["message"] = (result == ESP_OK) ? "Scan initiated" : "Scan failed";
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// Handle /data endpoint - returns first controller data in controller.h format
void handleData() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  
  // Find first active controller
  int firstActive = -1;
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    if (controllers[i].active && millis() - controllers[i].lastSeen < 10000) {
      firstActive = i;
      break;
    }
  }
  
  DynamicJsonDocument doc(4096);
  
  if (firstActive >= 0) {
    BMSData* data = &controllers[firstActive].data;
    doc["connected"] = true;
    doc["voltage"] = data->totalVoltage;
    doc["current"] = data->current;
    doc["soc"] = data->soc;
    doc["capacity"] = data->capacityRemaining;
    doc["cycles"] = data->cycleCount;
    doc["cellCount"] = data->cellCount;
    doc["productionDate"] = String(data->productionDate);
    doc["softwareVersion"] = data->softwareVersion;
    doc["hardwareVersion"] = String(data->hardwareVersion);
    doc["fetStatus"] = data->fetStatus;
    doc["fetStatusDesc"] = String("Charge: ") + ((data->fetStatus & 0x01) ? "On" : "Off") + 
                            ", Discharge: " + ((data->fetStatus & 0x02) ? "On" : "Off");
    doc["protectionStatusDesc"] = "OK"; // Simplified for hub
    
    JsonArray cells = doc.createNestedArray("cells");
    for (int i = 0; i < data->cellCount && i < 24; i++) {
      cells.add(data->cellVoltages[i]);
    }
    
    JsonArray temps = doc.createNestedArray("temperatures");
    for (int i = 0; i < data->tempSensorCount && i < 6; i++) {
      temps.add(data->temperatures[i]);
    }
  } else {
    doc["connected"] = false;
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleWiFiScan() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  Serial.println("[API] /api/wifi/scan requested");
  
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

// Debug endpoint to check controller registration status
void handleDebug() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  
  DynamicJsonDocument doc(4096);
  doc["wifiChannel"] = WiFi.channel();
  doc["uptime"] = millis() / 1000;
  doc["freeHeap"] = ESP.getFreeHeap();
  
  JsonArray allControllers = doc.createNestedArray("allControllers");
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    JsonObject ctrl = allControllers.createNestedObject();
    ctrl["slot"] = i;
    ctrl["active"] = controllers[i].active;
    char macStr[18];
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
            controllers[i].mac[0], controllers[i].mac[1], controllers[i].mac[2],
            controllers[i].mac[3], controllers[i].mac[4], controllers[i].mac[5]);
    ctrl["mac"] = macStr;
    ctrl["lastSeen"] = controllers[i].lastSeen > 0 ? (millis() - controllers[i].lastSeen) / 1000 : -1;
    ctrl["hasData"] = controllers[i].data.soc > 0;
    ctrl["voltage"] = controllers[i].data.totalVoltage;
    ctrl["soc"] = controllers[i].data.soc;
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleWiFiConfig() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  Serial.println("[API] /api/wifi POST requested");
  
  if (server.hasArg("ssid") && server.hasArg("password")) {
    wifi_ssid = server.arg("ssid");
    wifi_password = server.arg("password");
    
    prefs.begin("wificfg", false);
    prefs.putString("ssid", wifi_ssid);
    prefs.putString("password", wifi_password);
    prefs.end();
    
    wifiConfigured = true;
    
    // Try to connect
    connectToWiFi(wifi_ssid.c_str(), wifi_password.c_str());
    
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing parameters\"}");
  }
}

// ============== SETUP & LOOP ==============
void hubSetup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n========================================");
  Serial.println("ESP32 BMS Hub (Mode: HUB)");
  Serial.println("========================================");
  
  // Load WiFi config
  prefs.begin("wificfg", true);
  wifi_ssid = prefs.getString("ssid", "");
  wifi_password = prefs.getString("password", "");
  prefs.end();
  
  if (wifi_ssid.length() > 0) {
    wifiConfigured = true;
    Serial.println("WiFi configured: " + wifi_ssid);
  } else {
    Serial.println("WiFi not configured - use web interface");
  }
  
  // Init display
  initHubDisplay();
  
  // Load saved controllers
  loadControllers();
  
  // Init WiFi - try STA first, fallback to AP
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  
  // Try to connect to WiFi if configured
  if (wifiConfigured) {
    connectToWiFi(wifi_ssid.c_str(), wifi_password.c_str());
  } else {
    // No WiFi config - start AP immediately
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    WiFi.softAP(hub_ssid, hub_password, ESPNOW_CHANNEL, false, 4);
    Serial.printf("[WiFi] AP mode (no config): %s\n", hub_ssid);
  }
  
  // Important: ESP-NOW must be initialized AFTER WiFi is fully configured
  delay(100); // Give WiFi time to stabilize
  
  // ESP-NOW requires AP+STA mode to work properly
  WiFi.mode(WIFI_AP_STA);
  delay(50);
  
  // Re-enable AP if we were in STA mode (ESP-NOW needs it)
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.softAP(hub_ssid, hub_password, ESPNOW_CHANNEL, false, 4);
  }
  
  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    tft.setTextColor(TFT_RED);
    tft.drawString("ESP-NOW ERROR!", 160, 200);
    return;
  }
  Serial.println("[ESP-NOW] Initialized successfully");
  
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);
  
  // Add broadcast peer for discovery
  uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddr, 6);
  // Use current WiFi channel or default
  int initChannel = WiFi.channel();
  if (initChannel == 0) initChannel = ESPNOW_CHANNEL;
  peerInfo.channel = initChannel;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
  
  Serial.printf("[ESP-NOW] Initialized on channel %d\n", initChannel);
  Serial.println("Web Server started at http://192.168.4.1");
  Serial.println("Localtunnel: DISABLED for testing");
  Serial.println("========================================");
  Serial.println("Waiting for controllers...");
  Serial.println("========================================");
  
  // Init Web Server
  server.on("/", handleRoot);
  server.on("/api/data", handleApiData);
  server.on("/data", handleData);  // For controller.h web UI compatibility
  server.on("/api/controllers/scan", HTTP_POST, handleControllerScan);
  server.on("/api/wifi/scan", handleWiFiScan);
  server.on("/api/wifi", HTTP_POST, handleWiFiConfig);
  server.on("/api/debug", handleDebug);  // Debug endpoint
  server.begin();
  
  // Init controllers array
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    controllers[i].active = false;
    memset(controllers[i].mac, 0, 6);
  }
}

void hubLoop() {
  server.handleClient();
  updateDisplay();
}

#endif // HUB_H
