// display_controller.h - Display functions for BMS Controller
#ifndef DISPLAY_CONTROLLER_H
#define DISPLAY_CONTROLLER_H

#include <Arduino.h>
#include "display_base.h"

// BMS Data Structure (must match controller.h)
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
  uint16_t balanceStatus;
  uint16_t balanceStatusHigh;
  uint16_t protectionStatus;
  uint8_t fetStatus;
  uint8_t softwareVersion;
  uint8_t tempSensorCount;
  String productionDate;
  String hardwareVersion;
};

// Global BMS data instance (defined in controller.h)
extern BMSData bmsData;

// Display instance (defined in controller.h)
extern LGFX_MGT020 tft;

// Function from controller.h
String getProtectionStatusDescription(uint16_t status);

// Initialize controller display
void initControllerDisplay() {
  tft.init();
  tft.setRotation(0);  // Portrait mode
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(middle_center);
  tft.drawString("BMS Controller", 160, 120);
}

// Draw rounded rectangle frame
void drawFrame(int x, int y, int w, int h, uint32_t color) {
  tft.drawRoundRect(x, y, w, h, 5, color);
}

// Draw SOC progress bar
void drawProgressBar(int x, int y, int w, int h, int percent, uint32_t color) {
  tft.drawRoundRect(x, y, w, h, 3, TFT_WHITE);
  int fillW = (w - 4) * percent / 100;
  if (fillW > 0) {
    tft.fillRoundRect(x + 2, y + 2, fillW, h - 4, 2, color);
  }
}

// Draw grid for screen diagnostic - 320x240 LANDSCAPE
void drawGrid() {
  // Outer border
  tft.drawRect(0, 0, 320, 240, TFT_RED);
  
  // Vertical lines every 40px
  for (int x = 40; x < 320; x += 40) {
    tft.drawFastVLine(x, 0, 240, TFT_DARKGREY);
  }
  
  // Horizontal lines every 40px
  for (int y = 40; y < 240; y += 40) {
    tft.drawFastHLine(0, y, 320, TFT_DARKGREY);
  }
  
  // Center cross
  tft.drawFastVLine(160, 0, 240, TFT_YELLOW);
  tft.drawFastHLine(0, 120, 320, TFT_YELLOW);
  
  // Labels
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(top_left);
  tft.drawString("0,0", 5, 5);
  tft.setTextDatum(top_right);
  tft.drawString("320,0", 315, 5);
  tft.setTextDatum(bottom_left);
  tft.drawString("0,240", 5, 235);
  tft.setTextDatum(bottom_right);
  tft.drawString("320,240", 315, 235);
  tft.setTextDatum(middle_center);
  tft.setTextColor(TFT_YELLOW);
  tft.drawString("160,120", 160, 120);
}

// Update display with BMS data (rotation set in init) - ROTATED 90deg
void updateControllerDisplay(struct BMSData& data, bool connected, bool hubConfigured) {
  tft.fillScreen(TFT_BLACK);
  
  // Draw main frame border
  tft.drawRect(0, 0, 320, 240, TFT_BLUE);
  tft.drawRect(1, 1, 318, 238, TFT_DARKGREY);
  
  char buf[64];
  
  // Always show WiFi info at bottom
  tft.setTextDatum(bottom_left);
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN);
  if (WiFi.status() == WL_CONNECTED) {
    String ssid = WiFi.SSID();
    if (ssid.length() > 12) ssid = ssid.substring(0, 12);
    sprintf(buf, "W:%s Ch:%d", ssid.c_str(), WiFi.channel());
    tft.drawString(buf, 5, 225);
    sprintf(buf, "IP:%s", WiFi.localIP().toString().c_str());
    tft.drawString(buf, 5, 238);
  } else {
    tft.drawString("WiFi: AP Mode", 5, 225);
    sprintf(buf, "IP:%s Ch:%d", WiFi.softAPIP().toString().c_str(), WiFi.channel());
    tft.drawString(buf, 5, 238);
  }
  
  if (!connected) {
    tft.setTextDatum(middle_center);
    tft.setTextColor(TFT_RED);
    tft.drawString("BMS Not Connected", 160, 120);
    return;
  }
  
  tft.setTextDatum(top_left);
  
  // === LEFT COLUMN ===
  int x = 5, y = 5;
  int colW = 150;
  
  // Voltage & SOC
  drawFrame(x, y, colW, 50, TFT_BLUE);
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  sprintf(buf, "%.2fV", data.totalVoltage);
  tft.drawString(buf, x + 10, y + 5);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  sprintf(buf, "SOC:%d%%", data.soc);
  tft.drawString(buf, x + 10, y + 35);
  drawProgressBar(x + 80, y + 35, 60, 10, data.soc, data.soc > 60 ? TFT_GREEN : data.soc > 30 ? TFT_YELLOW : TFT_RED);
  y += 55;
  
  // Current & Power
  drawFrame(x, y, colW, 30, TFT_DARKGREY);
  float power = data.totalVoltage * data.current;
  sprintf(buf, "%.2fA %.1fW", data.current, power);
  tft.setTextColor(power > 0 ? TFT_GREEN : TFT_RED);
  tft.drawString(buf, x + 10, y + 8);
  y += 35;
  
  // Capacity & Cycles
  drawFrame(x, y, colW, 30, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE);
  sprintf(buf, "%.1f/%.1fAh", data.capacityRemaining, data.capacityTotal);
  tft.drawString(buf, x + 10, y + 5);
  sprintf(buf, "Cyc:%d", data.cycleCount);
  tft.drawString(buf, x + 100, y + 5);
  y += 35;
  
  // Cells
  drawFrame(x, y, colW, 45, TFT_DARKGREEN);
  if (data.cellCount > 0) {
    float minV = 5.0, maxV = 0.0;
    for (int i = 0; i < data.cellCount && i < 24; i++) {
      if (data.cellVoltages[i] < minV) minV = data.cellVoltages[i];
      if (data.cellVoltages[i] > maxV) maxV = data.cellVoltages[i];
    }
    float delta = maxV - minV;
    tft.setTextColor(TFT_WHITE);
    sprintf(buf, "C:%d Min:%.2f", data.cellCount, minV);
    tft.drawString(buf, x + 10, y + 5);
    sprintf(buf, "Max:%.2f d:%.0fmV", maxV, delta * 1000);
    tft.setTextColor(delta > 0.05 ? TFT_RED : TFT_GREEN);
    tft.drawString(buf, x + 10, y + 25);
  }
  y += 50;
  
  // Temps
  drawFrame(x, y, colW, 30, TFT_DARKCYAN);
  if (data.tempSensorCount > 0) {
    tft.setTextColor(TFT_WHITE);
    sprintf(buf, "T:%.1fC", data.temperatures[0]);
    tft.drawString(buf, x + 10, y + 8);
    if (data.tempSensorCount > 1) {
      sprintf(buf, "T2:%.1fC", data.temperatures[1]);
      tft.drawString(buf, x + 80, y + 8);
    }
  }
  
  // === RIGHT COLUMN ===
  x = 160; y = 5;
  
  // MOSFET Status
  drawFrame(x, y, colW, 60, TFT_DARKGREY);
  tft.setTextColor(data.fetStatus & 0x01 ? TFT_GREEN : TFT_RED);
  sprintf(buf, "CHG: %s", data.fetStatus & 0x01 ? "ON" : "OFF");
  tft.drawString(buf, x + 15, y + 10);
  tft.setTextColor(data.fetStatus & 0x02 ? TFT_GREEN : TFT_RED);
  sprintf(buf, "DSG: %s", data.fetStatus & 0x02 ? "ON" : "OFF");
  tft.drawString(buf, x + 15, y + 35);
  y += 65;
  
  // Hub & WiFi Info
  drawFrame(x, y, colW, 70, TFT_DARKGREY);
  tft.setTextColor(hubConfigured ? TFT_GREEN : TFT_ORANGE);
  tft.drawString(hubConfigured ? "HUB OK" : "NO HUB", x + 15, y + 10);
  
  // WiFi info
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(1);
  if (WiFi.status() == WL_CONNECTED) {
    String ssid = WiFi.SSID();
    if (ssid.length() > 10) ssid = ssid.substring(0, 10);
    sprintf(buf, "W:%s", ssid.c_str());
    tft.drawString(buf, x + 15, y + 30);
    sprintf(buf, "IP:%s", WiFi.localIP().toString().c_str());
    tft.drawString(buf, x + 15, y + 50);
  } else {
    tft.drawString("WiFi: AP Mode", x + 15, y + 30);
    sprintf(buf, "IP:%s", WiFi.softAPIP().toString().c_str());
    tft.drawString(buf, x + 15, y + 50);
  }
  
  bool balancing = (data.balanceStatus != 0 || data.balanceStatusHigh != 0);
  if (balancing) {
    tft.setTextColor(TFT_YELLOW);
    tft.drawString("BALANCING!", x + 80, y + 30);
  }
  y += 75;
  
  // Alert or Info
  if (data.protectionStatus != 0) {
    drawFrame(x, y, colW, 70, TFT_RED);
    tft.setTextColor(TFT_RED);
    tft.drawString("ALERT!", x + 15, y + 5);
    String prot = getProtectionStatusDescription(data.protectionStatus);
    tft.setTextColor(TFT_WHITE);
    if (prot.length() > 22) prot = prot.substring(0, 22);
    tft.drawString(prot.c_str(), x + 15, y + 30);
  } else {
    drawFrame(x, y, colW, 70, TFT_DARKGREY);
    tft.setTextColor(TFT_LIGHTGREY);
    sprintf(buf, "v%d", data.softwareVersion);
    tft.drawString(buf, x + 15, y + 5);
    tft.drawString(data.productionDate.c_str(), x + 50, y + 5);
    String hw = data.hardwareVersion;
    if (hw.length() > 20) hw = hw.substring(0, 20);
    tft.drawString(hw.c_str(), x + 15, y + 40);
  }
}

#endif // DISPLAY_CONTROLLER_H
