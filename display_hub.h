// display_hub.h - Display functions for BMS Hub
#ifndef DISPLAY_HUB_H
#define DISPLAY_HUB_H

#include "display_base.h"
#include <WiFi.h>

// Display instance for hub (defined in hub.h)
extern LGFX_MGT020 tft;

// Data structures (must match hub.h)
struct BMSData {
  float totalVoltage;
  float current;
  float capacityRemaining;
  float capacityTotal;
  uint16_t cycleCount;
  uint8_t soc;
  uint8_t cellCount;
  float cellVoltages[24];
  float temperatures[6];
  uint16_t protectionStatus;
  uint8_t fetStatus;
  uint8_t tempSensorCount;
  uint8_t softwareVersion;
  char productionDate[12];
  char hardwareVersion[32];
  bool connected;
  uint32_t timestamp;
  uint8_t ip[4];  // Controller IP address
};

struct ControllerData {
  uint8_t mac[6];
  uint32_t lastSeen;
  BMSData data;
  bool active;
};

// ============== DISPLAY FUNCTIONS ==============

void drawGrid() {
  tft.drawRect(0, 0, 320, 240, TFT_RED);
  for (int x = 40; x < 320; x += 40) tft.drawFastVLine(x, 0, 240, TFT_DARKGREY);
  for (int y = 40; y < 240; y += 40) tft.drawFastHLine(0, y, 320, TFT_DARKGREY);
  tft.drawFastVLine(160, 0, 240, TFT_YELLOW);
  tft.drawFastHLine(0, 120, 320, TFT_YELLOW);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(top_left); tft.drawString("0,0", 5, 5);
  tft.setTextDatum(top_right); tft.drawString("320,0", 315, 5);
  tft.setTextDatum(bottom_left); tft.drawString("0,240", 5, 235);
  tft.setTextDatum(bottom_right); tft.drawString("320,240", 315, 235);
}

// Initialize hub display
void initHubDisplay() {
  tft.init();
  tft.setRotation(0);  // Portrait - 0 at top-left
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(middle_center);
  tft.drawString("BMS Hub", 160, 120);
}

// Update hub display with aggregated data
void updateHubDisplay(int activeControllerCount, int maxControllers,
                       float totalVoltage, float totalCurrent, int avgSOC,
                       ControllerData controllers[], int maxControllersCount) {
  float power = totalVoltage * totalCurrent;
  
  // LCD display
  tft.startWrite();
  tft.fillScreen(TFT_BLACK);
  
  // Draw main frame border
  tft.drawRect(0, 0, 320, 240, TFT_BLUE);
  tft.drawRect(1, 1, 318, 238, TFT_DARKGREY);
  
  // Use built-in font
  tft.setTextDatum(top_center);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.drawString("BMS Hub Aggregator", 160, 5);
  
  // Active controllers count
  tft.setTextSize(2);
  tft.setTextColor(activeControllerCount > 0 ? TFT_GREEN : TFT_RED);
  char buf[32];
  sprintf(buf, "Active: %d/%d", activeControllerCount, maxControllers);
  tft.drawString(buf, 160, 35);
  
  // Voltage and Current
  tft.setTextDatum(top_left);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  sprintf(buf, "Voltage: %.2fV", totalVoltage);
  tft.drawString(buf, 10, 65);
  sprintf(buf, "Current: %.1fA", totalCurrent);
  tft.drawString(buf, 10, 85);
  
  // Power
  tft.setTextColor(power > 0 ? TFT_GREEN : (power < 0 ? TFT_RED : TFT_WHITE));
  sprintf(buf, "Power: %.0fW", power);
  tft.drawString(buf, 170, 65);
  
  // SOC
  tft.setTextColor(avgSOC > 50 ? TFT_GREEN : (avgSOC > 20 ? TFT_YELLOW : TFT_RED));
  sprintf(buf, "SOC: %d%%", avgSOC);
  tft.drawString(buf, 170, 85);
  
  // Controllers list
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(1);
  int y = 115;
  for (int i = 0; i < maxControllersCount && y < 220; i++) {
    if (controllers[i].active) {
      char name[20];
      sprintf(name, "C%d:%.1fV %.0fA %d%%", 
              i, controllers[i].data.totalVoltage,
              controllers[i].data.current, controllers[i].data.soc);
      tft.drawString(name, 10, y);
      y += 15;
    }
  }
  
  // WiFi status
  tft.setTextDatum(bottom_left);
  tft.setTextSize(1);
  if (WiFi.status() == WL_CONNECTED) {
    // Show SSID, channel and IP when connected
    tft.setTextColor(TFT_GREEN);
    String ssid = WiFi.SSID();
    if (ssid.length() > 10) ssid = ssid.substring(0, 10);
    sprintf(buf, "%s Ch:%d", ssid.c_str(), WiFi.channel());
    tft.drawString(buf, 10, 225);
    tft.setTextColor(TFT_CYAN);
    sprintf(buf, "%s", WiFi.localIP().toString().c_str());
    tft.drawString(buf, 10, 238);
  } else {
    // AP mode info
    tft.setTextColor(TFT_ORANGE);
    sprintf(buf, "AP Mode Ch:%d", WiFi.channel());
    tft.drawString(buf, 10, 225);
    tft.setTextColor(TFT_CYAN);
    sprintf(buf, "%s", WiFi.softAPIP().toString().c_str());
    tft.drawString(buf, 10, 238);
  }
  
  tft.endWrite();
}

#endif // DISPLAY_HUB_H
