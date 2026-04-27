#include "display.h" 
#include "main.h"   
#include "gbw.h"
#include "controller_lvgl_ui.h"
#include "controller_board.h"
#include "definitions.h"

#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ST7789.h> // Hardware-specific library for ST7789
#include <SPI.h>             // Arduino SPI library
#include <math.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include "comm.h"
#include "motor_hal.h"
#include "radio_mode.h"

#define COLOR_NAVY 0x0010
#define COLOR_BRIGHT_BLUE 0x55FF
#define COLOR_GREEN 0x4fe9
#define COLOR_TEAL 0x0438
#define COLOR_RED 0xe803
#define COLOR_YELLOW 0xe7e0

const int SCREEN_WIDTH = 280;
const int SCREEN_HEIGHT = 240;
Adafruit_ST7789 *tft = NULL;
const int HEADER_HEIGHT = 24;

#define SCREEN_IDLE      0
#define SCREEN_GBW_IDLE  1
#define SCREEN_GRINDING  2
#define SCREEN_PURGING   3
#define SCREEN_MENU         4
#define SCREEN_GRINDING_GBW 5
#define SCREEN_CALIBRATING  6
#define SCREEN_ERROR        7
#define SCREEN_MOTOR_SELECT 8

int initializedScreenId = -1;
bool dimmed = false;
bool disp_updateRequired = true;
unsigned long drawnErrorAt;
bool displayDisabled = false;
uint8_t appliedBrightness = 0;

const char* getCommStatusText(SYSTEM_STATUS status);
uint16_t getCommStatusColor(SYSTEM_STATUS status);

namespace {
const GFXfont* const HEADER_FONT = &FreeSans12pt7b;
const GFXfont* const SCALE_NAME_FONT = &FreeSans9pt7b;
const GFXfont* const WIFI_STATUS_FONT = &FreeSans9pt7b;
constexpr unsigned long MOTOR_SPLASH_DURATION_MS = 5000;
constexpr unsigned long MOTOR_SPLASH_VISIBLE_MS = 1800;
constexpr unsigned long MOTOR_SPLASH_BLANK_MS = 250;
String motorSplashLabel = "";
unsigned long motorSplashStartedAt = 0;
unsigned long motorSplashUntil = 0;

bool isJmcBaudMenuItem(int selectedIdx) {
  return selectedIdx >= Menu4Items::JMCBAUD9600 && selectedIdx <= Menu4Items::JMCBAUD115200;
}

uint32_t baudForJmcMenuItem(int selectedIdx) {
  switch (selectedIdx) {
    case Menu4Items::JMCBAUD9600: return 9600;
    case Menu4Items::JMCBAUD19200: return 19200;
    case Menu4Items::JMCBAUD38400: return 38400;
    case Menu4Items::JMCBAUD57600: return 57600;
    case Menu4Items::JMCBAUD115200: return 115200;
    default: return 0;
  }
}

int getMotorSplashTextSize(const String& label) {
  if (label.length() == 0) return 1;

  constexpr int horizontalMargin = 8;
  constexpr int verticalMargin = 12;
  const int maxWidth = SCREEN_WIDTH - (horizontalMargin * 2);
  const int maxHeight = SCREEN_HEIGHT - (verticalMargin * 2);
  const int sizeByWidth = maxWidth / (6 * label.length());
  const int sizeByHeight = maxHeight / 8;
  const int textSize = min(sizeByWidth, sizeByHeight);
  return textSize > 0 ? textSize : 1;
}

bool hasSplashMaskPixel(const GFXcanvas1& canvas, int16_t x, int16_t y, int16_t radius = 0) {
  for (int16_t dy = -radius; dy <= radius; ++dy) {
    const int16_t yy = y + dy;
    if (yy < 0 || yy >= SCREEN_HEIGHT) continue;
    for (int16_t dx = -radius; dx <= radius; ++dx) {
      const int16_t xx = x + dx;
      if (xx < 0 || xx >= SCREEN_WIDTH) continue;
      if (canvas.getPixel(xx, yy)) {
        return true;
      }
    }
  }
  return false;
}

void drawMotorSplashLabel(const String& label) {
  const int textSize = getMotorSplashTextSize(label);
  GFXcanvas1 canvas(SCREEN_WIDTH, SCREEN_HEIGHT);

  if (canvas.getBuffer() == nullptr) {
    tft->setFont(NULL);
    tft->setTextWrap(false);
    tft->setTextSize(textSize);
    tft->setTextColor(COLOR_YELLOW);
    int16_t x1, y1;
    uint16_t w, h;
    tft->getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
    tft->setCursor((SCREEN_WIDTH - w) / 2, (SCREEN_HEIGHT - h) / 2);
    tft->print(label);
    return;
  }

  canvas.fillScreen(0);
  canvas.setFont(NULL);
  canvas.setTextWrap(false);
  canvas.setTextSize(textSize);
  canvas.setTextColor(1);

  int16_t x1, y1;
  uint16_t w, h;
  canvas.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
  const int textX = (SCREEN_WIDTH - w) / 2;
  const int textY = (SCREEN_HEIGHT - h) / 2;
  canvas.setCursor(textX, textY);
  canvas.print(label);

  const int stripePitch = max(10, textSize);
  constexpr int shadowOffset = 4;
  constexpr int strokeRadius = 1;

  for (int16_t y = 0; y < SCREEN_HEIGHT; ++y) {
    for (int16_t x = 0; x < SCREEN_WIDTH; ++x) {
      if (!hasSplashMaskPixel(canvas, x, y, strokeRadius)) continue;

      const int16_t shadowX = x + shadowOffset;
      const int16_t shadowY = y + shadowOffset;
      if (shadowX < SCREEN_WIDTH && shadowY < SCREEN_HEIGHT) {
        tft->drawPixel(shadowX, shadowY, COLOR_BRIGHT_BLUE);
      }

      const uint16_t stripeColor = (((x + y) / stripePitch) % 2 == 0) ? COLOR_BRIGHT_BLUE : COLOR_NAVY;
      tft->drawPixel(x, y, stripeColor);
    }
  }
}

bool drawMotorProbeSplashIfActive() {
  static int lastFlashPhase = -1;
  static String lastDrawnLabel = "";

  if (motorSplashUntil == 0 || motorSplashLabel.length() == 0) {
    return false;
  }

  const unsigned long now = millis();
  if (now >= motorSplashUntil) {
    motorSplashUntil = 0;
    motorSplashStartedAt = 0;
    motorSplashLabel = "";
    lastFlashPhase = -1;
    lastDrawnLabel = "";
    tft->setTextSize(1);
    tft->setTextWrap(true);
    initializedScreenId = -1;
    disp_updateRequired = true;
    return false;
  }

  const unsigned long flashCycleMs = MOTOR_SPLASH_VISIBLE_MS + MOTOR_SPLASH_BLANK_MS;
  const unsigned long splashElapsed = now - motorSplashStartedAt;
  const int flashPhase = ((splashElapsed % flashCycleMs) < MOTOR_SPLASH_VISIBLE_MS) ? 1 : 0;
  if (flashPhase != lastFlashPhase || motorSplashLabel != lastDrawnLabel) {
    tft->fillScreen(ST77XX_BLACK);

    if (flashPhase == 1) {
      drawMotorSplashLabel(motorSplashLabel);
    }

    lastFlashPhase = flashPhase;
    lastDrawnLabel = motorSplashLabel;
  }

  tft->setTextSize(1);
  tft->setTextWrap(true);
  disp_updateRequired = true;
  return true;
}

String getScaleDisplayLabel() {
  String label = "";

  if (scaleMutex != NULL && xSemaphoreTake(scaleMutex, pdMS_TO_TICKS(5))) {
    label = scale_name;
    if (label.length() == 0) label = scale_connect_name;
    xSemaphoreGive(scaleMutex);
  }

  label.trim();
  if (label.length() == 0) label = "Scale";
  return label;
}

String fitLabelToFont(const String& text, int maxWidth, const GFXfont* font) {
  if (text.length() == 0) return text;

  int16_t x1, y1;
  uint16_t w, h;
  tft->setFont(font);
  tft->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  if (w <= maxWidth) return text;

  String fitted = text;
  while (fitted.length() > 4) {
    fitted = fitted.substring(0, fitted.length() - 1);
    const String candidate = fitted + "...";
    tft->getTextBounds(candidate, 0, 0, &x1, &y1, &w, &h);
    if (w <= maxWidth) {
      return candidate;
    }
  }

  return text.substring(0, 1);
}

String fitHeaderLabel(const String& text, int maxWidth) {
  return fitLabelToFont(text, maxWidth, HEADER_FONT);
}

String fitScaleNameLabel(const String& text, int maxWidth) {
  return fitLabelToFont(text, maxWidth, SCALE_NAME_FONT);
}

String getMotorDisplayLabel() {
  return String(motor_get_type_label());
}

uint32_t getActiveMotorCommBaud() {
  if (commStatus != COMM_CONNECTED) return 0;

  switch (comm_get_profile()) {
    case COMM_PROFILE_JMC:
      return comm_get_jmc_baud();
    case COMM_PROFILE_KRS:
      return COMM_BAUD_RATE_KRS;
    case COMM_PROFILE_RT:
    default:
      return COMM_BAUD_RATE_RT;
  }
}

bool shouldShowMotorBaudBadge(SYSTEM_STATUS status) {
  return status == MOTOR_READY && getActiveMotorCommBaud() > 0;
}

void drawMotorStatusLine(int statusX, int statusY, SYSTEM_STATUS status) {
  tft->setFont(&FreeSans9pt7b);
  tft->fillRect(statusX - 2, statusY - 18, 118, 24, ST77XX_BLACK);

  if (shouldShowMotorBaudBadge(status)) {
    const int iconX = statusX + 2;
    const int iconY = statusY - 15;
    tft->drawLine(iconX, iconY + 8, iconX + 5, iconY + 13, COLOR_GREEN);
    tft->drawLine(iconX + 5, iconY + 13, iconX + 16, iconY + 2, COLOR_GREEN);

    char baudStr[16];
    snprintf(baudStr, sizeof(baudStr), "%lu", static_cast<unsigned long>(getActiveMotorCommBaud()));
    tft->setTextColor(COLOR_GREEN);
    tft->setCursor(statusX + 24, statusY);
    tft->print(baudStr);
    return;
  }

  tft->setTextColor(getCommStatusColor(status));
  tft->setCursor(statusX, statusY);
  tft->print(getCommStatusText(status));
}

int getMotorDirectionIndicator() {
  if (motor_currentRPM > 0) return 1;
  if (motor_currentRPM < 0) return -1;
  const int16_t effectiveSetRPM = motor_get_effective_set_rpm();
  if (effectiveSetRPM > 0) return 1;
  if (effectiveSetRPM < 0) return -1;
  return 0;
}

int16_t getDisplayMotorRPM() {
  if (motor_currentRPM != 0) return motor_currentRPM;

  switch (state) {
    case GRINDING:
    case PURGING:
    case GRINDING_GBW:
    case CALIBRATING:
      return motor_get_effective_set_rpm();
    default:
      return 0;
  }
}

const char* getAnimatedDirectionText(int direction, uint8_t phase) {
  static const char* const cwFrames[] = {">  ", ">> ", ">>>", " >>"};
  static const char* const ccwFrames[] = {"  <", " <<", "<<<", "<< "};

  if (direction > 0) return cwFrames[phase % 4];
  if (direction < 0) return ccwFrames[phase % 4];
  return "";
}

bool getHeaderLineBounds(int rightX, int baselineY, const String& text, const GFXfont* font, int16_t* x, int16_t* y, uint16_t* w, uint16_t* h) {
  if (text.length() == 0 || x == nullptr || y == nullptr || w == nullptr || h == nullptr) {
    return false;
  }

  int16_t x1, y1;
  uint16_t width, height;

  tft->setFont(font);
  tft->getTextBounds(text, 0, baselineY, &x1, &y1, &width, &height);
  const int drawX = rightX - width;
  tft->getTextBounds(text, drawX, baselineY, x, y, w, h);
  return true;
}

void clearRightAlignedLine(int rightX, int baselineY, const String& text, const GFXfont* font) {
  int16_t x, y;
  uint16_t w, h;
  if (!getHeaderLineBounds(rightX, baselineY, text, font, &x, &y, &w, &h)) {
    return;
  }

  tft->fillRect(x - 2, y - 1, w + 4, h + 2, ST77XX_BLACK);
}

void drawRightAlignedLine(int rightX, int baselineY, const String& text, const GFXfont* font, uint16_t textColor) {
  int16_t x, y;
  uint16_t w, h;
  if (!getHeaderLineBounds(rightX, baselineY, text, font, &x, &y, &w, &h)) {
    return;
  }

  tft->setFont(font);
  tft->setTextColor(textColor);
  tft->setCursor(x, baselineY);
  tft->print(text);
}

bool getLeftLineBounds(int leftX, int baselineY, const String& text, const GFXfont* font, int16_t* x, int16_t* y, uint16_t* w, uint16_t* h) {
  if (text.length() == 0 || x == nullptr || y == nullptr || w == nullptr || h == nullptr) {
    return false;
  }

  tft->setFont(font);
  tft->getTextBounds(text, leftX, baselineY, x, y, w, h);
  return true;
}

void clearLeftAlignedLine(int leftX, int baselineY, const String& text, const GFXfont* font) {
  int16_t x, y;
  uint16_t w, h;
  if (!getLeftLineBounds(leftX, baselineY, text, font, &x, &y, &w, &h)) {
    return;
  }

  tft->fillRect(x - 2, y - 1, w + 4, h + 2, ST77XX_BLACK);
}

void drawLeftAlignedLine(int leftX, int baselineY, const String& text, const GFXfont* font, uint16_t textColor) {
  int16_t x, y;
  uint16_t w, h;
  if (!getLeftLineBounds(leftX, baselineY, text, font, &x, &y, &w, &h)) {
    return;
  }

  tft->setFont(font);
  tft->setTextColor(textColor);
  tft->setCursor(leftX, baselineY);
  tft->print(text);
}

void drawRightAlignedPill(int rightX, int baselineY, const String& text, const GFXfont* font, uint16_t textColor, uint16_t pillColor) {
  int16_t x, y;
  uint16_t w, h;
  if (!getHeaderLineBounds(rightX, baselineY, text, font, &x, &y, &w, &h)) {
    return;
  }

  constexpr int horizontalPadding = 10;
  constexpr int verticalPadding = 4;
  constexpr int radius = 9;
  const int pillX = x - horizontalPadding;
  const int pillY = y - verticalPadding;
  const int pillW = w + horizontalPadding * 2;
  const int pillH = h + verticalPadding * 2;

  tft->fillRoundRect(pillX, pillY, pillW, pillH, radius, pillColor);
  tft->drawRoundRect(pillX, pillY, pillW, pillH, radius, COLOR_BRIGHT_BLUE);
  tft->setFont(font);
  tft->setTextColor(textColor);
  tft->setCursor(x, baselineY);
  tft->print(text);
}

void drawTopIndicators(int screenId) {
  static int prevScreenId = -1;
  static String prevMotorLabel = "";
  static String prevWifiLabel = "";
  static uint16_t prevWifiColor = 0;

  const bool forceRedraw = prevScreenId != screenId;
  const int leftMargin = 8;
  // Pull the motor badge slightly inward so enclosure lips do not clip the
  // right edge of the text on the physical screen.
  const int rightMargin = 30;
  // Reserve only as much of the header as the short motor badge needs so the
  // Wi-Fi status line has enough room for a full IPv4 address.
  const int regionWidth = 88;
  const int indicatorClearHeight = HEADER_HEIGHT + 4;
  // Drop the header text a little farther so the enclosure lip does not clip it.
  const int motorBaselineY = 22;
  const int wifiBaselineY = 22;
  const int rightX = SCREEN_WIDTH - rightMargin;
  const int regionX = SCREEN_WIDTH - (regionWidth + 12);
  const int leftRegionWidth = regionX - leftMargin - 8;
  const String motorLabel = fitHeaderLabel(getMotorDisplayLabel(), regionWidth);
  const String wifiLabel = fitLabelToFont(radio_mode_lcd_status(), leftRegionWidth, WIFI_STATUS_FONT);
  const uint16_t wifiColor = radio_mode_status_ok() ? COLOR_GREEN : COLOR_YELLOW;
  const bool motorChanged = forceRedraw || motorLabel != prevMotorLabel;
  const bool wifiChanged = forceRedraw || wifiLabel != prevWifiLabel || wifiColor != prevWifiColor;

  if (motorChanged) {
    tft->fillRect(regionX, 0, SCREEN_WIDTH - regionX, indicatorClearHeight, ST77XX_BLACK);
    drawRightAlignedPill(rightX, motorBaselineY, motorLabel, HEADER_FONT, COLOR_BRIGHT_BLUE, COLOR_NAVY);
  }
  if (wifiChanged) {
    clearLeftAlignedLine(leftMargin, wifiBaselineY, prevWifiLabel, WIFI_STATUS_FONT);
    clearLeftAlignedLine(leftMargin, wifiBaselineY, wifiLabel, WIFI_STATUS_FONT);
    drawLeftAlignedLine(leftMargin, wifiBaselineY, wifiLabel, WIFI_STATUS_FONT, wifiColor);
  }

  prevScreenId = screenId;
  prevMotorLabel = motorLabel;
  prevWifiLabel = wifiLabel;
  prevWifiColor = wifiColor;
}

void drawScaleNameLegend(const String& label, const String& previousLabel) {
  const int baselineY = SCREEN_HEIGHT * 0.82;
  const int rightX = SCREEN_WIDTH - 10;

  if (label == previousLabel) {
    return;
  }

  clearRightAlignedLine(rightX, baselineY, previousLabel, SCALE_NAME_FONT);
  clearRightAlignedLine(rightX, baselineY, label, SCALE_NAME_FONT);
  drawRightAlignedLine(rightX, baselineY, label, SCALE_NAME_FONT, COLOR_BRIGHT_BLUE);
}

void drawSeparatorLine(int x, int y, int width, bool animated) {
  constexpr int lineHeight = 6;
  constexpr int lineRadius = 3;
  constexpr int dotWidth = 14;
  constexpr int dotGap = 8;
  constexpr unsigned long stepMs = 80;

  static unsigned long lastStepAt = 0;
  static int phase = 0;

  if (animated && millis() - lastStepAt >= stepMs) {
    phase = (phase + 3) % (dotWidth + dotGap);
    lastStepAt = millis();
  }

  tft->fillRoundRect(x, y, width, lineHeight, lineRadius, ST77XX_BLACK);

  if (animated) {
    for (int dotX = x - phase; dotX < x + width; dotX += dotWidth + dotGap) {
      const int drawX = dotX < x ? x : dotX;
      const int drawEnd = (dotX + dotWidth) > (x + width) ? (x + width) : (dotX + dotWidth);
      const int drawWidth = drawEnd - drawX;
      if (drawWidth > 0) {
        tft->fillRoundRect(drawX, y, drawWidth, lineHeight, lineRadius, COLOR_NAVY);
      }
    }
    disp_updateRequired = true;
    return;
  }

  tft->fillRoundRect(x, y, width, lineHeight, lineRadius, COLOR_NAVY);
}
}


void display_init() { 
  if(!controller_board_has_display()) {
  displayDisabled = true;
  dimmed = false;
  appliedBrightness = 0;
  return;
  }

  const ControllerBoardPins& pins = controller_board_pins();
  SPIClass *spi = new SPIClass(HSPI);
  spi->begin(pins.displaySclk, -1, pins.displaySda, pins.displayCs);
  tft = new Adafruit_ST7789(spi, pins.displayCs, pins.displayDc, pins.displayReset);
  // 80MHz should work, but may need lower speeds if attached to the motor
  tft->setSPISpeed(80000000);
  // this will vary depending on display type
  tft->init(SCREEN_HEIGHT, SCREEN_WIDTH, SPI_MODE0);
  tft->setRotation(1); // Landscape

  // Set backlight pin as output and turn on backlight
  //pinMode(DISP_BL, OUTPUT);
  ledcAttach(pins.displayBacklight, 4000, 8);
  ledcWrite(pins.displayBacklight, 0);
  appliedBrightness = 0;
  displayDisabled = false;
  controller_lvgl_init(tft, SCREEN_WIDTH, SCREEN_HEIGHT);
  update_display();
}

void setBrightness() { 
  if(!controller_board_has_display()) {
  dimmed = false;
  return;
  }

  const ControllerBoardPins& pins = controller_board_pins();
  uint8_t newBrightness;
  static int dimTime = 15; 

  if(lastActivity + dimTime * 60000 < millis()) { 
    newBrightness = 1;
    dimmed = true;
  } else {
      newBrightness = Menu1[DISP_BRIGHTNESS].value;
      dimmed = false;
  }

  if(appliedBrightness != newBrightness) { 
    ledcWrite(pins.displayBacklight, newBrightness);
    appliedBrightness = newBrightness;
  }
}

uint16_t getGrindingRpmColor() {
  return motor_is_direction_reversed() ? COLOR_YELLOW : COLOR_GREEN;
}

void update_display() { 
  if(!controller_board_has_display()) {
  disp_updateRequired = false;
  return;
  }

  static unsigned long lastUpdate;
  if(newData == true) disp_updateRequired = true;
  if(displayDisabled) return;

  if(disp_updateRequired != true) return; //This is mostly functional for testing.
  if(lastUpdate + 1000/DISP_REFRESH_RATE > millis()) return;

  lastUpdate = millis();
  disp_updateRequired = false;
  setBrightness();
  controller_lvgl_sync();
}

void display_off() {
    if(!controller_board_has_display()) {
    displayDisabled = true;
    return;
    }

    const ControllerBoardPins& pins = controller_board_pins();
    if(displayDisabled) {
        return;
    }

    if (tft) {
        tft->fillScreen(ST77XX_BLACK);
        tft->enableDisplay(false); // turn off display output (if supported)
    }
    controller_lvgl_set_output_enabled(false);
    ledcWrite(pins.displayBacklight,0);
    appliedBrightness = 0;
    displayDisabled = true;
}

void display_on() {
    if(!controller_board_has_display()) {
    displayDisabled = false;
    dimmed = false;
    disp_updateRequired = false;
    return;
    }

    if(!displayDisabled) {
        setBrightness();
        disp_updateRequired = true;
        return;
    }

    displayDisabled = false;
    if (tft) {
        tft->enableDisplay(true);
        tft->fillScreen(ST77XX_BLACK);
    }
    controller_lvgl_set_output_enabled(true);
    controller_lvgl_invalidate();
    initializedScreenId = -1;
    appliedBrightness = 0;
    setBrightness();
    disp_updateRequired = true;
}

void display_show_motor_probe_splash(const char* motorLabel) {
  if (motorLabel == nullptr || motorLabel[0] == '\0') {
    return;
  }
  controller_lvgl_show_motor_probe_splash(motorLabel);
  disp_updateRequired = true;
}

const char* getCommStatusText(SYSTEM_STATUS status) {
  switch (status) {
    case MOTOR_NOT_CONNECTED:return "Disconnected";
    case MOTOR_NOT_READY:    return "Starting up..";
    case MOTOR_READY:        return "Connected";
    case MOTOR_ENABLED:      return "Enabled";
    case MOTOR_FAULT:        return "Fault";
    default:           return "UNKNOWN";
  }
}

uint16_t getCommStatusColor(SYSTEM_STATUS status) {
  switch (status) {
    case MOTOR_NOT_CONNECTED: return COLOR_RED;
    case MOTOR_NOT_READY: return COLOR_YELLOW;
    case MOTOR_READY: return COLOR_GREEN;
    case MOTOR_ENABLED: return COLOR_GREEN;
    case MOTOR_FAULT: return COLOR_RED;
    default: return ST77XX_WHITE;
  }
}

const char* getScaleStatusText(SCALE_STATUS scaleStatus) {
  switch (scaleStatus) {
    case SCALE_DISCONNECTED: return "Disconnected";
    case SCALE_CONNECTED:        return "Connected";
    case SCALE_CONNECTING:    return "Connecting..";
    case SCALE_LOST:        return "Lost..";
    default:           return "UNKNOWN";
  }
}

uint16_t getScaleStatusColor(SCALE_STATUS status) {
  switch (status) {
    case SCALE_CONNECTED: return COLOR_GREEN;
    case SCALE_DISCONNECTED: return COLOR_RED;
    case SCALE_CONNECTING: return COLOR_YELLOW;
    case SCALE_LOST: return COLOR_YELLOW;
    default: return ST77XX_WHITE;
  }
}

const char* getErrorChar(uint8_t error) {
  switch(error) {
    case 1: return "Drive connection failed";
    case 2: return "Drive connection lost";
    case 3: return "Drive fault:";
    case 4: return "drive packet loss > 25";
    case 5: return "Failed to create mutex";
    case 6: return "Motor stalled..";
    case 7: return "Write error, please reboot all";
    case 8: return "First use, reboot drive";
    case 100: return "Unexpected set RPM";
    case 101: return "No drive connection";
    case 102: return "Calibration cancelled";
    case 103: return "Could not load stored data";
    case 104: return "Scale connection lost";
    case 105: return "No output after 3s";
    case 106: return "No scale connected";
    case 107: return "Please Reboot Drive";
    default: return "Unknown Error";
  }
}


void drawErrorOverlay() {
  const int OVERLAY_MARGIN_X = 5;  // extra left/right margin for rounded corners
  const int OVERLAY_HEIGHT  = 44;   // total height of overlay area
  const int OVERLAY_Y      = HEADER_HEIGHT;     // keep header visible
  static uint8_t prevError = 0;  // Track the last drawn overlay error
  static unsigned long errorDrawnAt;

  if(errorDrawnAt + 5000 < millis() && error > 100 && prevError == error) { 
    error = 0;
  }

  if (prevError == error) return;

  if (error == 0) { 
    prevError = 0;
    initializedScreenId = -1;
    disp_updateRequired = true;
    return;
  }

  prevError = error;

  // DRAW ERROR:
  errorDrawnAt = millis();

  // Clear overlay area
  tft->fillRect(
    OVERLAY_MARGIN_X, 
    OVERLAY_Y, 
    SCREEN_WIDTH - 2 * OVERLAY_MARGIN_X, 
    OVERLAY_HEIGHT, 
    ST77XX_BLACK
  );
  
  // --- Prepare error text ---
  const char* errMsg = getErrorChar(error);
  tft->setFont(&FreeSans9pt7b); // Small readable font

  // Get text bounds
  int16_t x1, y1;
  uint16_t w, h;
  tft->getTextBounds(errMsg, 0, 0, &x1, &y1, &w, &h);

  // Centered X, placed vertically in middle of overlay area
  int textX = SCREEN_WIDTH/2 - w/2;
  int textY = OVERLAY_Y + 18; 

  // Background rectangle: add a little extra width for padding
  int bgPad = 8;
  tft->fillRoundRect(
    textX - bgPad, 
    OVERLAY_Y + 2, 
    w + 2*bgPad, 
    OVERLAY_HEIGHT - 4, 
    8,   // roundness
    ST77XX_RED
  );

  tft->setTextColor(ST77XX_WHITE, ST77XX_RED);
  tft->setCursor(textX, textY);
  tft->print(errMsg);

  //Add error code if drive error;
  if(error == 3) {
  char hexBuf[10];
    sprintf(hexBuf, "0x%02X", errorCode);
    tft->setFont(&FreeSans9pt7b);
    tft->getTextBounds(hexBuf, 0, 0, &x1, &y1, &w, &h);
    int codeX = SCREEN_WIDTH/2 - w/2;
    int codeY = OVERLAY_Y + OVERLAY_HEIGHT - 6;
    tft->setTextColor(ST77XX_WHITE, ST77XX_RED);
    tft->setCursor(codeX, codeY);
    tft->print(hexBuf);
  }
}



void drawIdleScreen() {
  static int prevSetRPM = -1;
  static SYSTEM_STATUS prevSysStatus = MOTOR_INVALID;
  

  int16_t x1, y1;
  uint16_t w, h;

  if (initializedScreenId != SCREEN_IDLE) {
    tft->fillScreen(ST77XX_BLACK);

    int lineY = SCREEN_HEIGHT * 0.7;
    int lineW = SCREEN_WIDTH * 0.8;
    int lineX = (SCREEN_WIDTH - lineW) / 2;
    tft->fillRoundRect(lineX, lineY, lineW, 6, 3, COLOR_NAVY);

    tft->setFont(&FreeSans12pt7b);
    tft->setTextColor(ST77XX_WHITE);
    tft->setCursor(SCREEN_WIDTH / 2 + 30, SCREEN_HEIGHT / 2 - 10);
    tft->print("RPM");

    initializedScreenId = SCREEN_IDLE;
    prevSetRPM = -1;
    prevSysStatus = MOTOR_INVALID;

    tft->setFont(&FreeSans9pt7b);
    tft->getTextBounds("Purge", 0, 0, &x1, &y1, &w, &h);

    tft->setCursor(SCREEN_WIDTH - 10 - w, SCREEN_HEIGHT*0.82);
    tft->setTextColor(ST77XX_WHITE);  
    tft->print("Purge");

    tft->setCursor(10, SCREEN_HEIGHT*0.82);
    tft->print("Drive");
    
    const char * purgeStatus;
    if(Menu2[AUTO_PURGE_ENABLED].value == true) { 
      purgeStatus = "Enabled";
    } else purgeStatus = "Disabled";

    tft->getTextBounds(purgeStatus, 0, 0, &x1, &y1, &w, &h);
    int valX = SCREEN_WIDTH - 10 - w;
    int valY = SCREEN_HEIGHT*0.91;

    tft->setCursor(valX, valY);
    tft->print(purgeStatus);
  }

  drawTopIndicators(SCREEN_IDLE);
  drawSeparatorLine((SCREEN_WIDTH - SCREEN_WIDTH * 0.8) / 2, SCREEN_HEIGHT * 0.7, SCREEN_WIDTH * 0.8, false);

  // --- Update setRPM ---
  if (prevSetRPM != setRPM) {
    char newVal[10];
    sprintf(newVal, "%d", setRPM);

    tft->setFont(&FreeSans24pt7b);
    tft->getTextBounds("8888", 0, 0, &x1, &y1, &w, &h); // Max 4 digits
    int valX = SCREEN_WIDTH / 2 - w;
    int valY = SCREEN_HEIGHT / 2 - 10;

    tft->fillRect(valX - 4, valY - h, w + 16, h + 12, ST77XX_BLACK);

    tft->getTextBounds(newVal, 0, 0, &x1, &y1, &w, &h); // Max 4 digits
    tft->setTextColor(ST77XX_WHITE);
    valX = SCREEN_WIDTH / 2 - w;
    tft->setCursor(valX, valY);
    tft->print(newVal);

    prevSetRPM = setRPM;
  }

  // --- Update commStatus ---
  if (prevSysStatus != currentStatus) {
    int statusY = SCREEN_HEIGHT * 0.91;
    int statusX = 10;
    drawMotorStatusLine(statusX, statusY, currentStatus);

    prevSysStatus = currentStatus;
  }
}

void drawGbWIdleScreen() {
  static float prevSetWeight = -1000;
  static float prevCurrentWeight = -1000;
  static SYSTEM_STATUS prevSysStatus = MOTOR_INVALID;
  static SCALE_STATUS prevScaleStatus = INVALID_SCALE_STATUS;
  static String prevScaleLegend = "";

  if (initializedScreenId != SCREEN_GBW_IDLE) {
    tft->fillScreen(ST77XX_BLACK);

    int lineY = SCREEN_HEIGHT * 0.7;
    int lineW = SCREEN_WIDTH * 0.8;
    int lineX = (SCREEN_WIDTH - lineW) / 2;
    tft->fillRoundRect(lineX, lineY, lineW, 6, 3, COLOR_NAVY);

    initializedScreenId = SCREEN_GBW_IDLE;
    prevSetWeight = -1.0;
    prevCurrentWeight = -1.0;
    prevSysStatus = MOTOR_INVALID;
    prevScaleStatus = INVALID_SCALE_STATUS;
    prevScaleLegend = "";

    tft->setFont(&FreeSans9pt7b);
    tft->setTextColor(ST77XX_WHITE);
    tft->setCursor(10, SCREEN_HEIGHT*0.82);
    tft->print("Drive");
  }

  drawTopIndicators(SCREEN_GBW_IDLE);
  const String scaleLegend = fitScaleNameLabel(getScaleDisplayLabel(), 140);
  drawScaleNameLegend(scaleLegend, prevScaleLegend);
  prevScaleLegend = scaleLegend;
  drawSeparatorLine((SCREEN_WIDTH - SCREEN_WIDTH * 0.8) / 2, SCREEN_HEIGHT * 0.7, SCREEN_WIDTH * 0.8, false);

  // --- SetWeight (centered) ---
  if (prevSetWeight != setWeight) {
    char newVal[10];
    sprintf(newVal, "%.1f g", setWeight/1000.0);

    tft->setFont(&FreeSans24pt7b);
    int16_t x1, y1;
    uint16_t w, h;
    tft->getTextBounds("88.8 g", 0, 0, &x1, &y1, &w, &h);
    int valX = SCREEN_WIDTH/2 - w/2;
    int valY = SCREEN_HEIGHT / 2 - 30;
    tft->fillRect(valX - 2, valY - h, w + 10, h + 14, ST77XX_BLACK);

    tft->setTextColor(ST77XX_WHITE);
    tft->setCursor(valX, valY);
    tft->print(newVal);
    prevSetWeight = setWeight;
  }

  // --- CurrentWeight (right lower) ---
  if (prevCurrentWeight != currentWeight) {
    char newVal[10];
    sprintf(newVal, "%.1f g", (currentWeight / 1000.0));

    tft->setFont(&FreeSans12pt7b);
    int16_t x1, y1;
    uint16_t w, h;
    tft->getTextBounds("-888.8 g", 0, 0, &x1, &y1, &w, &h);
    int valX = SCREEN_WIDTH/2 + 40;
    int valY = SCREEN_HEIGHT*0.7 - 12;

    tft->fillRect(valX - 4, valY - h, w + 10, h + 12, ST77XX_BLACK);

    tft->setTextColor(getScaleStatusColor(scaleStatus));
    tft->setCursor(valX, valY);
    tft->print(newVal);

    prevCurrentWeight = currentWeight;
  }

  // --- COMM status (bottom-left, larger font, more right, centered) ---
  if (prevSysStatus != currentStatus) {
    int statusY = SCREEN_HEIGHT * 0.91;
    int statusX = 10;
    drawMotorStatusLine(statusX, statusY, currentStatus);

    prevSysStatus = currentStatus;
  }

  // --- Scale status (bottom-right) ---
  if (prevScaleStatus != scaleStatus) {
    const char* newScaleText = getScaleStatusText(scaleStatus);

    tft->setFont(&FreeSans9pt7b);
    int16_t x1, y1; uint16_t w, h;
    tft->getTextBounds("Disconnected", 0, 0, &x1, &y1, &w, &h);
    int statusY = SCREEN_HEIGHT * 0.91;
    int statusX = SCREEN_WIDTH - 10 - w;
    tft->fillRect(statusX - 4, statusY - h, w + 10, h + 8, ST77XX_BLACK);

    tft->getTextBounds(newScaleText, 0, 0, &x1, &y1, &w, &h);
    statusY = SCREEN_HEIGHT * 0.91;
    statusX = SCREEN_WIDTH - 10 - w;

    tft->setTextColor(getScaleStatusColor(scaleStatus));
    tft->setCursor(statusX, statusY);
    tft->print(newScaleText);

    prevScaleStatus = scaleStatus;

    // Update weight too. 
    char newVal[10];
    sprintf(newVal, "%.1f g", (currentWeight / 1000.0));

    tft->setFont(&FreeSans12pt7b);
    tft->getTextBounds("-888.8 g", 0, 0, &x1, &y1, &w, &h);
    int valX = SCREEN_WIDTH/2 + 40;
    int valY = SCREEN_HEIGHT*0.7 - 12;

    tft->fillRect(valX - 4, valY - h, w + 10, h + 12, ST77XX_BLACK);

    tft->setTextColor(getScaleStatusColor(scaleStatus));
    tft->setCursor(valX, valY);
    tft->print(newVal);

    prevCurrentWeight = currentWeight;
  }
}

void drawGrindingOrPurgingScreen() {
    static int16_t prevRPM = -32768;
    static int16_t prevTorque = 0xFFFF;
    static SYSTEM_STATUS prevSysStatus = MOTOR_INVALID;
    static bool prevPurgeEnabled = false;
    static bool prevPurgeReady = false;
    static bool prevPurgingDrawn = false;
    static int8_t prevDirection = 2;
    static int8_t prevDirectionPhase = -1;

    int16_t x1, y1;
    uint16_t w, h;

    // Layout constants
    const int dividerY = SCREEN_HEIGHT * 0.7;
    const int dividerW = SCREEN_WIDTH * 0.8;
    const int dividerX = (SCREEN_WIDTH - dividerW) / 2;
    const int speedoCX = SCREEN_WIDTH - SCREEN_WIDTH / 4;
    const int speedoCY = dividerY / 2 + 10;
    const int speedoR = 60;

    int thisScreenId = SCREEN_GRINDING;

    if (initializedScreenId != SCREEN_GRINDING && initializedScreenId != SCREEN_PURGING) {
        tft->fillScreen(ST77XX_BLACK);
        tft->fillRoundRect(dividerX, dividerY, dividerW, 6, 3, COLOR_NAVY);
        initializedScreenId = SCREEN_GRINDING;
        prevRPM = -32768;
        prevTorque = 0xFFFF;
        prevSysStatus = MOTOR_INVALID;
        prevPurgeEnabled = !Menu2[AUTO_PURGE_ENABLED].value;
        prevPurgeReady = !ready_purge;
        prevPurgingDrawn = false;
        prevDirection = 2;
        prevDirectionPhase = -1;

        tft->setFont(&FreeSans9pt7b);
        tft->getTextBounds("Purge", 0, 0, &x1, &y1, &w, &h);

        tft->setCursor(SCREEN_WIDTH - 10 - w, SCREEN_HEIGHT*0.82);
        tft->setTextColor(ST77XX_WHITE);  
        tft->print("Purge");

        tft->setCursor(10, SCREEN_HEIGHT*0.82);
        tft->print("Drive");
    }

    drawTopIndicators(state == PURGING ? SCREEN_PURGING : SCREEN_GRINDING);
    drawSeparatorLine(dividerX, dividerY, dividerW, true);

    // --- Direction indicator (left side, green) ---
    const int8_t direction = getMotorDirectionIndicator();
    const int8_t directionPhase = direction == 0 ? 0 : ((millis() / 180) % 4);
    if (prevDirection != direction || prevDirectionPhase != directionPhase) {
      tft->setFont(&FreeSans12pt7b);
      tft->getTextBounds("<<<", 0, 0, &x1, &y1, &w, &h);
      const int dirX = 12;
      const int dirY = 64;
      tft->fillRect(dirX - 2, dirY - h, w + 12, h + 10, ST77XX_BLACK);

      if (direction != 0) {
        tft->setTextColor(COLOR_GREEN);
        tft->setCursor(dirX, dirY);
        tft->print(getAnimatedDirectionText(direction, directionPhase));
      }

      prevDirection = direction;
      prevDirectionPhase = directionPhase;
    }

    // --- RPM digits (left side) ---
    const int16_t displayRPM = abs((int)getDisplayMotorRPM());
    if (prevRPM != displayRPM) {
      char newVal[10];
      sprintf(newVal, "%d", displayRPM);

      tft->setFont(&FreeSans24pt7b);
      int16_t x1, y1;
      uint16_t w, h;
      tft->getTextBounds("-8888", 0, 0, &x1, &y1, &w, &h); // Max 4 digits
      int valX = 10;
      int valY = SCREEN_HEIGHT / 2 - 10;

      tft->fillRect(valX - 2, valY - h, w + 10, h + 8, ST77XX_BLACK);

      tft->setTextColor(direction == 0 ? ST77XX_WHITE : (state == PURGING ? COLOR_YELLOW : getGrindingRpmColor()));
      tft->setCursor(valX, valY);
      tft->print(newVal);

      prevRPM = displayRPM;
    }
  

    // --- Torque speedometer (right side) ---
    const int16_t torquePercent = motor_get_torque_percent();
    if (prevTorque != motor_currentTorque) {
        tft->fillCircle(speedoCX, speedoCY, speedoR + 2, ST77XX_BLACK);

        // Gradient color: green (0%) -> yellow (50%) -> red (100%)
        uint8_t r, g, b;
        if (abs(torquePercent) <= 50) {
            // Green to Yellow: (0,255,0) -> (255,255,0)
            r = (uint8_t)(255.0 * (abs(torquePercent) / 50.0));
            g = 255;
            b = 0;
        } else {
            // Yellow to Red: (255,255,0) -> (255,0,0)
            r = 255;
            g = (uint8_t)(255.0 * (1.0 - (abs(torquePercent) - 50) / 50.0));
            b = 0;
        }
        uint16_t color = tft->color565(r, g, b);

        float angle = (abs(torquePercent) / 100.0) * 270.0;
        for (int i = 0; i < angle; i += 4) {
            float rad = (135 + i) * M_PI / 180.0;
            int x0 = speedoCX + cos(rad) * (speedoR - 10);
            int y0 = speedoCY + sin(rad) * (speedoR - 10);
            int x1 = speedoCX + cos(rad) * speedoR;
            int y1 = speedoCY + sin(rad) * speedoR;
            tft->drawLine(x0, y0, x1, y1, color);
        }
        tft->drawCircle(speedoCX, speedoCY, speedoR, ST77XX_WHITE);

        tft->setFont(&FreeSans18pt7b);
        tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        char percentStr[8];
        sprintf(percentStr, "%d%%", torquePercent);
        int16_t x1, y1;
        uint16_t w, h;
        tft->getTextBounds(percentStr, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor(speedoCX - w / 2, speedoCY+ h/2);
        tft->print(percentStr);

        prevTorque = motor_currentTorque;
    }

    // --- Connection status (bottom left) ---
  if (prevSysStatus != currentStatus) {
    int statusY = SCREEN_HEIGHT * 0.91;
    int statusX = 10;
    drawMotorStatusLine(statusX, statusY, currentStatus);

    prevSysStatus = currentStatus;
  }

  const char* newPurgeText;

    // --- Purge status (bottom right) ---
    if (state == PURGING) {
        // Always show "Purging" in yellow
        if (!prevPurgingDrawn) {
            newPurgeText = "Purging";

            tft->setFont(&FreeSans9pt7b);
            int16_t x1, y1; uint16_t w, h;
            tft->getTextBounds("Detecting..", 0, 0, &x1, &y1, &w, &h);
            int statusY = SCREEN_HEIGHT * 0.91;
            int statusX = SCREEN_WIDTH - 10 - w;
            tft->fillRect(statusX - 4, statusY - h, w + 2, h + 8, ST77XX_BLACK);

            tft->getTextBounds(newPurgeText, 0, 0, &x1, &y1, &w, &h);
            statusY = SCREEN_HEIGHT * 0.91;
            statusX = SCREEN_WIDTH - 10 - w;

            tft->setTextColor(ST77XX_WHITE);
            tft->setCursor(statusX, statusY);
            tft->print(newPurgeText);
            prevPurgingDrawn = true;
        }

    } else {
        // Show regular purge status
        if (prevPurgeEnabled != Menu2[AUTO_PURGE_ENABLED].value || prevPurgeReady != ready_purge || initializedScreenId != SCREEN_GRINDING) {
            tft->setFont(&FreeSans9pt7b);

            
            int16_t x1, y1; uint16_t w, h;
            tft->getTextBounds("Detecting..", 0, 0, &x1, &y1, &w, &h);
            int statusY = SCREEN_HEIGHT * 0.91;
            int statusX = SCREEN_WIDTH - 10 - w;
            tft->fillRect(statusX - 4, statusY - h, w + 2, h + 8, ST77XX_BLACK);

            tft->setTextColor(Menu2[AUTO_PURGE_ENABLED].value ? (ready_purge ? COLOR_GREEN : COLOR_YELLOW) : ST77XX_WHITE);

            if (Menu2[AUTO_PURGE_ENABLED].value == true) { 
              if (ready_purge == true) newPurgeText = "Imminent!";
              else newPurgeText = "Detecting..";
            } else newPurgeText = "Disabled";

                        tft->getTextBounds(newPurgeText, 0, 0, &x1, &y1, &w, &h);
            statusY = SCREEN_HEIGHT * 0.91;
            statusX = SCREEN_WIDTH - 10 - w;

            tft->setTextColor(ST77XX_WHITE);
            tft->setCursor(statusX, statusY);
            tft->print(newPurgeText);

            prevPurgeEnabled = Menu2[AUTO_PURGE_ENABLED].value;
            prevPurgeReady = ready_purge;
            prevPurgingDrawn = false;
        }
    }
}

void drawGrindingGbwScreen() {
    static int16_t prevRPM = -32768;
    static int32_t prevCurrentWeight = -999999;
    static SYSTEM_STATUS prevSysStatus = MOTOR_INVALID;
    static SCALE_STATUS prevScaleStatus = INVALID_SCALE_STATUS;
    static String prevScaleLegend = "";

    // Layout constants
    const int dividerY = SCREEN_HEIGHT * 0.7;
    const int dividerW = SCREEN_WIDTH * 0.8;
    const int dividerX = (SCREEN_WIDTH - dividerW) / 2;
    const int rpmX = 20;
    const int rpmY = dividerY / 2 - 32;
    const int speedoCX = SCREEN_WIDTH - SCREEN_WIDTH / 4;
    const int speedoCY = dividerY / 2 + 10;
    const int speedoR = 60;

    int thisScreenId = SCREEN_GRINDING_GBW;
    if (initializedScreenId != thisScreenId) {
        tft->fillScreen(ST77XX_BLACK);
        tft->fillRoundRect(dividerX, dividerY, dividerW, 6, 3, COLOR_NAVY);

        tft->setFont(&FreeSans9pt7b);
        tft->setTextColor(ST77XX_WHITE);
        tft->setCursor(10, SCREEN_HEIGHT*0.82);
        tft->print("Drive");

        initializedScreenId = thisScreenId;
        prevRPM = -32768;
        prevCurrentWeight = -999999;
        prevSysStatus = MOTOR_INVALID;
        prevScaleStatus = INVALID_SCALE_STATUS;
        prevScaleLegend = "";
    }

    drawTopIndicators(thisScreenId);
    const String scaleLegend = fitScaleNameLabel(getScaleDisplayLabel(), 140);
    drawScaleNameLegend(scaleLegend, prevScaleLegend);
    prevScaleLegend = scaleLegend;
    drawSeparatorLine(dividerX, dividerY, dividerW, true);

    // --- RPM digits (left side) ---
      const int16_t displaySetRPM = abs(motor_get_effective_set_rpm());
      if (prevRPM != displaySetRPM) {
      char newVal[10];
      sprintf(newVal, "%d", displaySetRPM);

      tft->setFont(&FreeSans24pt7b);
      int16_t x1, y1;
      uint16_t w, h;
      tft->getTextBounds("8888", 0, 0, &x1, &y1, &w, &h); // Max 4 digits
      int valX = 20;
      int valY = SCREEN_HEIGHT / 2 - 10;

      tft->fillRect(valX - 2, valY - h, w + 4, h + 8, ST77XX_BLACK);

      tft->setTextColor(getGrindingRpmColor());
      tft->setCursor(valX, valY);
      tft->print(newVal);

      prevRPM = displaySetRPM;
    }

    // --- Speedometer (right side): progress from 0 to setWeight ---
    int32_t weight = currentWeight;
    int32_t goal = setWeight;
    if (goal == 0) goal = 1; // avoid div by zero
    float percent = constrain(float(weight) / float(goal), 0.0, 1.0);

    if (prevCurrentWeight != weight) {
        tft->fillCircle(speedoCX, speedoCY, speedoR + 2, ST77XX_BLACK);

        // Color: linear from white to green
        uint8_t g = 0xE9 * percent + 0xFF * (1 - percent); // green increases, white fades
        uint16_t color = tft->color565((uint8_t)(0xFF * (1 - percent)), g, (uint8_t)(0xFF * (1 - percent)));

        float angle = percent * 270.0;
        for (int i = 0; i < angle; i += 4) {
            float rad = (135 + i) * M_PI / 180.0;
            int x0 = speedoCX + cos(rad) * (speedoR - 10);
            int y0 = speedoCY + sin(rad) * (speedoR - 10);
            int x1 = speedoCX + cos(rad) * speedoR;
            int y1 = speedoCY + sin(rad) * speedoR;
            tft->drawLine(x0, y0, x1, y1, color);
        }
        tft->drawCircle(speedoCX, speedoCY, speedoR, ST77XX_WHITE);

        // Draw % in center
        tft->setFont(&FreeSans12pt7b);
        tft->setTextColor(color, ST77XX_BLACK);
        char weightStr[8];
        sprintf(weightStr, "%.1fg", (currentWeight / 1000.0));
        int16_t x1, y1;
        uint16_t w, h;
        tft->getTextBounds(weightStr, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor(speedoCX - w / 2, speedoCY + h/2);
        tft->print(weightStr);

        prevCurrentWeight = weight;
    }

    // --- COMM status (bottom-left, larger font, more right, centered) ---
    if (prevSysStatus != currentStatus) {
      int statusY = SCREEN_HEIGHT * 0.91;
      int statusX = 10;
      drawMotorStatusLine(statusX, statusY, currentStatus);

      prevSysStatus = currentStatus;
    }

  if (prevScaleStatus != scaleStatus) {
      const char* newScaleText = getScaleStatusText(scaleStatus);

      tft->setFont(&FreeSans9pt7b);
      int16_t x1, y1; uint16_t w, h;
      tft->getTextBounds("Connection Lost", 0, 0, &x1, &y1, &w, &h);
      int statusY = SCREEN_HEIGHT * 0.91;
      int statusX = SCREEN_WIDTH - 10 - w;
      tft->fillRect(statusX - 4, statusY - h, w + 2, h + 8, ST77XX_BLACK);

      tft->getTextBounds(newScaleText, 0, 0, &x1, &y1, &w, &h);
      statusY = SCREEN_HEIGHT * 0.91;
      statusX = SCREEN_WIDTH - 10 - w;

      tft->setTextColor(getScaleStatusColor(scaleStatus));
      tft->setCursor(statusX, statusY);
      tft->print(newScaleText);

      prevScaleStatus = scaleStatus;
  }
}

void drawCalibratingScreen() {
    static int16_t prevRPM = -32768;
    static int16_t prevTorque = 0xFFFF;
    static SYSTEM_STATUS prevSysStatus = MOTOR_INVALID;
    static int prevStep = -1;
    static int prevTotal = -1;

    // Layout constants
    const int dividerY = SCREEN_HEIGHT * 0.7;
    const int dividerW = SCREEN_WIDTH * 0.8;
    const int dividerX = (SCREEN_WIDTH - dividerW) / 2;
    const int speedoCX = SCREEN_WIDTH - SCREEN_WIDTH / 4;
    const int speedoCY = dividerY / 2 + 10;
    const int speedoR = 60;


    // Use global variables directly, do NOT redeclare or define them here!
    // calibrateArray and currentCal are declared extern in RT/motorcontrol_rt.h
    int totalSteps = 1 + SIZEOFCALIBRATEARRAY / sizeof(uint16_t);
    int step = currentCal + 1; // Show as 1-based index

    // Draw divider and clear if needed
    if (initializedScreenId != SCREEN_CALIBRATING) {
        tft->fillScreen(ST77XX_BLACK);
        tft->fillRoundRect(dividerX, dividerY, dividerW, 6, 3, COLOR_NAVY);

        tft->setFont(&FreeSans9pt7b);
        tft->setTextColor(ST77XX_WHITE);  
        tft->setCursor(10, SCREEN_HEIGHT*0.82);
        tft->print("Drive");

        const char * calibrateText = "Calibrating.."; 
        tft->setFont(&FreeSans18pt7b);
        int16_t x1, y1;
        uint16_t w, h;
        tft->getTextBounds(calibrateText, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor(SCREEN_WIDTH / 2 - w/2, 10 + h);
        tft->print(calibrateText);

        initializedScreenId = SCREEN_CALIBRATING;
        prevRPM = -32768;
        prevTorque = 0xFFFF;
        prevSysStatus = MOTOR_INVALID;
        prevStep = -1;
        prevTotal = -1;
    }

    drawTopIndicators(SCREEN_CALIBRATING);
    drawSeparatorLine(dividerX, dividerY, dividerW, true);

    // --- RPM digits (left side) ---
    const int16_t displayRPM = getDisplayMotorRPM();
    if (prevRPM != displayRPM) {
      char newVal[10];
      sprintf(newVal, "%d", displayRPM);

      tft->setFont(&FreeSans24pt7b);
      int16_t x1, y1;
      uint16_t w, h;
      tft->getTextBounds("8888", 0, 0, &x1, &y1, &w, &h); // Max 4 digits
      int valX = 10;
      int valY = SCREEN_HEIGHT / 2 - 10;

      tft->fillRect(valX - 2, valY - h, w + 10, h + 8, ST77XX_BLACK);

      tft->setTextColor(ST77XX_WHITE);
      tft->setCursor(valX, valY);
      tft->print(newVal);

      prevRPM = displayRPM;
    }

    // --- Torque speedometer (right side) ---
    int16_t torquePercent = motor_currentTorque;
    if (prevTorque != motor_currentTorque) {
        tft->setFont(&FreeSans18pt7b);
        tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        int16_t x1, y1;
        uint16_t w, h;
        const char* clearStr = "-8888";
        tft->getTextBounds(clearStr, 0, 0, &x1, &y1, &w, &h);
        int valX = speedoCX - w / 2;
        int valY = speedoCY + h / 2;
        tft->getTextBounds(clearStr, valX, valY, &x1, &y1, &w, &h);
        tft->fillRect(x1 - 4, y1 - 4, w + 8, h + 8, ST77XX_BLACK);

        char percentStr[8];
        sprintf(percentStr, "%d", torquePercent);

        tft->getTextBounds(percentStr, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor(speedoCX - w / 2, speedoCY + h / 2);
        tft->print(percentStr);

        prevTorque = motor_currentTorque;
    }


    // --- COMM status (bottom left) ---
    if (prevSysStatus != currentStatus) {
      int statusY = SCREEN_HEIGHT * 0.91;
      int statusX = 10;
      drawMotorStatusLine(statusX, statusY, currentStatus);

      prevSysStatus = currentStatus;
    }

    // --- Calibration progress (bottom right) ---
    if (prevStep != step) {

        int16_t x1, y1;
        uint16_t w, h;
        tft->setFont(&FreeSans18pt7b);
        tft->setTextColor(ST77XX_WHITE);
        tft->getTextBounds("888 / 888", 0, 0, &x1, &y1, &w, &h);

        int statusX = SCREEN_WIDTH - 20 -w;
        int statusY = SCREEN_HEIGHT*0.9;
        tft->fillRect(statusX - 1, statusY - h, w + 12, h + 8, ST77XX_BLACK);

        char stepStr[16];
        sprintf(stepStr, "%d / %d", step, totalSteps);
        tft->getTextBounds(stepStr, 0, 0, &x1, &y1, &w, &h);
        tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        tft->setCursor(SCREEN_WIDTH - 20 - w, SCREEN_HEIGHT*0.90);
        tft->print(stepStr);

        prevStep = step;
    }
}

// Helper to get menu pointer by number
static const menuEntry* getMenuByNum(int menuNum) {
    switch(menuNum) {
        case 1: return Menu1;
        case 2: return Menu2;
        case 3: return Menu3;
        case 4: return Menu4;
        default: return nullptr;
    }
}

static const char* getMotorSelectLabel(int idx) {
    switch(idx) {
        case MOTOR_SELECT_AUTO: return "AUTO";
        case MOTOR_SELECT_JMC: return "JMC";
        case MOTOR_SELECT_RT: return "RT";
        case MOTOR_SELECT_KRS_130ST_M07725: return "KRS";
        default: return "";
    }
}

static const char* getMotorSelectHint(int idx) {
    switch(idx) {
        case MOTOR_SELECT_AUTO: return "Retry auto probe";
        case MOTOR_SELECT_JMC: return "Force JMC backend";
        case MOTOR_SELECT_RT: return "Force RT backend";
        case MOTOR_SELECT_KRS_130ST_M07725: return "Force KRS 130ST backend";
        default: return "";
    }
}

void drawMotorSelectScreen() {
    static int prevSelected = -1;
    const int thisScreenId = SCREEN_MOTOR_SELECT;

    if(initializedScreenId != thisScreenId) {
        tft->fillScreen(ST77XX_BLACK);
        initializedScreenId = thisScreenId;
        prevSelected = -1;

        tft->setFont(&FreeSans18pt7b);
        tft->setTextColor(ST77XX_WHITE);
        int16_t x1, y1;
        uint16_t w, h;
        const char* title = "Select Drive";
        tft->getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, HEADER_HEIGHT + h + 8);
        tft->print(title);

        tft->setFont(&FreeSans9pt7b);
        const char* line1 = "Auto probe did not lock.";
        const char* line2 = "Rotate to choose, press encoder to apply.";
        const char* line3 = "Press Start to go back.";
        tft->getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, 74);
        tft->print(line1);
        tft->getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, 96);
        tft->print(line2);
        tft->getTextBounds(line3, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, 116);
        tft->print(line3);
    }

    drawTopIndicators(thisScreenId);

    if(prevSelected == motorSelectSelected) {
        return;
    }

    tft->fillRect(16, 126, SCREEN_WIDTH - 32, 96, ST77XX_BLACK);

    for(int i = 0; i < NUM_MOTOR_SELECT_ITEMS; ++i) {
        const bool selected = i == motorSelectSelected;
        const int boxX = 26;
        const int boxY = 132 + i * 30;
        const int boxW = SCREEN_WIDTH - 52;
        const int boxH = 24;

        tft->drawRoundRect(boxX, boxY, boxW, boxH, 8, selected ? COLOR_GREEN : COLOR_NAVY);

        tft->setFont(selected ? &FreeSans12pt7b : &FreeSans9pt7b);
        tft->setTextColor(selected ? COLOR_BRIGHT_BLUE : ST77XX_WHITE);

        int16_t x1, y1;
        uint16_t w, h;
        tft->getTextBounds(getMotorSelectLabel(i), 0, 0, &x1, &y1, &w, &h);
        const int labelX = boxX + 10;
        const int labelY = boxY + boxH - 6;
        tft->setCursor(labelX, labelY);
        tft->print(getMotorSelectLabel(i));

        tft->setFont(&FreeSans9pt7b);
        tft->setTextColor(selected ? COLOR_GREEN : 0x8410);
        tft->getTextBounds(getMotorSelectHint(i), 0, 0, &x1, &y1, &w, &h);
        tft->setCursor(boxX + boxW - w - 10, labelY);
        tft->print(getMotorSelectHint(i));
    }

    prevSelected = motorSelectSelected;
}

void drawMenuScreen(int menuNum, int numItems, int selectedIdx) {
    static int prevSelectedIdx = -1;
    static int prevMenuNum = -1;
    static char prevNames[5][21] = {0}; // 20 chars + null, for 5 visible items

    const menuEntry* menu = getMenuByNum(menuNum);

    // Layout constants
    const int itemCount = 5; // Always show 5 items
    const int centerY = SCREEN_HEIGHT / 2;
    const int itemHeight = 44;
    const int fadeColor = 0x8410; // 50% gray (faded)
    const int normalColor = ST77XX_WHITE;
    const int selectedColor = COLOR_GREEN;
    const int leftEdge = 10;
    const int centerX = SCREEN_WIDTH / 2;

    int thisScreenId = menuNum * 1000; // Use menuNum as unique ID

    bool fullRedraw = false;
    if (initializedScreenId != thisScreenId) {
        tft->fillScreen(ST77XX_BLACK);
        initializedScreenId = thisScreenId;
        prevSelectedIdx = -1; // force redraw
        prevMenuNum = menuNum;
        memset(prevNames, 0, sizeof(prevNames));
        fullRedraw = true;
    }

    if (prevSelectedIdx != selectedIdx) {
        fullRedraw = true;
        prevSelectedIdx = selectedIdx;
    }

    if (prevMenuNum != menuNum) {
        fullRedraw = true;
        prevMenuNum = menuNum;
    }

    drawTopIndicators(thisScreenId);

    for (int i = 0; i < itemCount; ++i) {
        int relIdx = i - itemCount / 2; // -2, -1, 0, 1, 2
        int menuIdx = (selectedIdx + relIdx + numItems) % numItems;

        int y = centerY + relIdx * itemHeight;

        // X position: move selected more to the left
        int x;
        if (relIdx == 0) x = leftEdge + 40;
        else if (abs(relIdx) == 1) x = leftEdge + 25;
        else x = leftEdge;

        uint16_t color = normalColor;
        if (relIdx == 0) color = selectedColor;
        else if (abs(relIdx) == 2) color = fadeColor;

        // Font: scale down selected, use FreeSans fonts
        const GFXfont* font;
        if (relIdx == 0) font = &FreeSans12pt7b;
        else font = &FreeSans9pt7b;

        char currName[21];
        strncpy(currName, menu[menuIdx].name, 20);
        currName[20] = '\0';

        char* nl = strchr(currName, '\n');
        if (nl) *nl = '\0';

        bool needsUpdate = fullRedraw || strcmp(prevNames[i], currName) != 0;

        if (needsUpdate) {
            int16_t x1, y1;
            uint16_t w, h;
            tft->setFont(font);
            tft->getTextBounds(prevNames[i], x, y, &x1, &y1, &w, &h);
            tft->fillRect(x1 - 10, y1 - 6, w + 20, h + 12, ST77XX_BLACK);

            tft->setFont(font);
            tft->setTextColor(color, ST77XX_BLACK);
            tft->setCursor(x, y);
            tft->print(currName);

            if (relIdx == 0) {
                tft->getTextBounds(currName, x, y, &x1, &y1, &w, &h);
                tft->drawRoundRect(x1 - 8, y1 - 4, w + 16, h + 8, 6, selectedColor);
            }

            strncpy(prevNames[i], currName, 20);
            prevNames[i][20] = '\0';
        }
    }
}

void drawMenuValueScreen(int menuNum, int selectedIdx) {
    // Clear screen if needed
    static int prevSelectedIdx = -1, prevMenuNum = -1;
    static int16_t prevValue = 0xFFFF;
    int thisScreenId = (menuNum << 8) | selectedIdx;
    
    const menuEntry* menu = getMenuByNum(menuNum);
    const menuEntry& item = menu[selectedIdx];

    if (initializedScreenId != thisScreenId) {
        tft->fillScreen(ST77XX_BLACK);
        initializedScreenId = thisScreenId;
        prevSelectedIdx = 0xFFFF;
        prevMenuNum = 0xFFFF;
        prevValue = 0xFFFF; // invalid so redraw

      // Special case: Calibrate (Menu1[CALIBRATE])
      if (menuNum == 1 && selectedIdx == Menu1Items::CALIBRATE) {
        // "Calibrate?" top center
        tft->setFont(&FreeSans18pt7b);
        tft->setTextColor(ST77XX_WHITE);
        int16_t x1, y1; uint16_t w, h;
        tft->getTextBounds("Calibrate?", 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, HEADER_HEIGHT + h + 4);
        tft->print("Calibrate?");
        
        // Instruction below
        tft->setFont(&FreeSans9pt7b);
        const char* msg1 = "Long press the encoder button to";
        const char* msg2 = "start the calibration process";
        const char* msg3 = "Press Start to exit";
        tft->getTextBounds(msg1, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, 100);
        tft->print(msg1);
        tft->getTextBounds(msg2, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, 120);
        tft->print(msg2);
        tft->getTextBounds(msg3, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, 140);
        tft->print(msg3);
      } else if (menuNum == 4 && isJmcBaudMenuItem(selectedIdx)) {
        tft->setFont(&FreeSans18pt7b);
        tft->setTextColor(ST77XX_WHITE);
        int16_t x1, y1; uint16_t w, h;
        char title[24];
        snprintf(title, sizeof(title), "JMC %lu?", static_cast<unsigned long>(baudForJmcMenuItem(selectedIdx)));
        tft->getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, HEADER_HEIGHT + h + 4);
        tft->print(title);

        tft->setFont(&FreeSans9pt7b);
        char msg1[48];
        char msg2[48];
        snprintf(msg1, sizeof(msg1), "Set drive comm to %lu profile.", static_cast<unsigned long>(baudForJmcMenuItem(selectedIdx)));
        snprintf(msg2, sizeof(msg2), "Board stores %lu for next boot.", static_cast<unsigned long>(baudForJmcMenuItem(selectedIdx)));
        const char* msg3 = "Press encoder. Then power cycle.";
        const char* msg4 = "Press Start to exit";
        tft->getTextBounds(msg1, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, 92);
        tft->print(msg1);
        tft->getTextBounds(msg2, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, 112);
        tft->print(msg2);
        tft->getTextBounds(msg3, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, 132);
        tft->print(msg3);
        tft->getTextBounds(msg4, 0, 0, &x1, &y1, &w, &h);
        tft->setCursor((SCREEN_WIDTH - w) / 2, 152);
        tft->print(msg4);
      } else { 
          // Show menu item name, top center
          tft->setFont(&FreeSans12pt7b);
          tft->setTextColor(ST77XX_WHITE);
          int16_t x1, y1; uint16_t w, h;
          tft->getTextBounds(item.name, 0, 0, &x1, &y1, &w, &h);
          tft->setCursor((SCREEN_WIDTH - w) / 2, HEADER_HEIGHT + h + 2);
          tft->print(item.name);
      }
    } 

    drawTopIndicators(thisScreenId);
    if(menuNum == 1 && selectedIdx == Menu1Items::CALIBRATE) prevValue = item.value; //basically do nothing
    else if(menuNum == 4 && isJmcBaudMenuItem(selectedIdx)) {
      static JmcBaudTrialStatus prevTrialStatus = JMC_BAUD_SWITCH_FAIL_LOST;
      static uint32_t prevTrialBaud = 0xFFFFFFFFUL;
      static uint32_t prevReadBeforeBaud = 0xFFFFFFFFUL;
      static uint32_t prevReadAfterBaud = 0xFFFFFFFFUL;
      static uint32_t prevTargetBaud = 0xFFFFFFFFUL;
      static bool prevBaudConnected = false;
      static String prevUartLabel = "";
      static String prevDriveLabel = "";

      const JmcBaudTrialStatus trialStatus = motor_get_jmc_baud_trial_status();
      const uint32_t trialBaud = motor_get_jmc_baud_trial_result_baud();
      const uint32_t readBeforeBaud = motor_get_jmc_baud_trial_read_before();
      const uint32_t readAfterBaud = motor_get_jmc_baud_trial_read_after();
      const String uartLabel = comm_get_profile_label();
      const String driveLabel = motor_get_type_label();
      const uint32_t targetBaud = baudForJmcMenuItem(selectedIdx);
      const bool baudConnected =
          motor_get_drive_type() == DRIVE_TYPE_JMC &&
          commStatus == COMM_CONNECTED &&
          comm_get_jmc_baud() == targetBaud;

      if(prevValue == 0xFFFF || prevTrialStatus != trialStatus || prevTrialBaud != trialBaud || prevReadBeforeBaud != readBeforeBaud || prevReadAfterBaud != readAfterBaud || prevTargetBaud != targetBaud || prevBaudConnected != baudConnected || prevUartLabel != uartLabel || prevDriveLabel != driveLabel) {
        tft->fillRect(0, 166, SCREEN_WIDTH, SCREEN_HEIGHT - 166, ST77XX_BLACK);

        uint16_t statusColor = ST77XX_WHITE;
        switch(trialStatus) {
          case JMC_BAUD_SWITCH_RUNNING:
            statusColor = COLOR_YELLOW;
            break;
          case JMC_BAUD_SWITCH_SET_OK:
          case JMC_BAUD_SWITCH_ALREADY_SET:
            statusColor = COLOR_GREEN;
            break;
          case JMC_BAUD_SWITCH_FAIL_NO_JMC:
          case JMC_BAUD_SWITCH_FAIL_WRITE:
          case JMC_BAUD_SWITCH_FAIL_VERIFY:
          case JMC_BAUD_SWITCH_FAIL_LOST:
            statusColor = COLOR_RED;
            break;
          case JMC_BAUD_SWITCH_IDLE:
          default:
            statusColor = ST77XX_WHITE;
            break;
        }

        char detailLine[48];
        snprintf(detailLine, sizeof(detailLine), "Drive: %s   UART: %s", driveLabel.c_str(), uartLabel.c_str());
        char baudLine[48];
        if(trialBaud > 0) snprintf(baudLine, sizeof(baudLine), "Result baud: %lu", static_cast<unsigned long>(trialBaud));
        else snprintf(baudLine, sizeof(baudLine), "Result baud: none");
        char readLine[48];
        if(readBeforeBaud > 0 && readAfterBaud > 0) {
          snprintf(readLine, sizeof(readLine), "Read: %lu -> %lu",
                   static_cast<unsigned long>(readBeforeBaud),
                   static_cast<unsigned long>(readAfterBaud));
        } else if(readBeforeBaud > 0) {
          snprintf(readLine, sizeof(readLine), "Read before: %lu",
                   static_cast<unsigned long>(readBeforeBaud));
        } else if(readAfterBaud > 0) {
          snprintf(readLine, sizeof(readLine), "Read after: %lu",
                   static_cast<unsigned long>(readAfterBaud));
        } else {
          snprintf(readLine, sizeof(readLine), "Read: unavailable");
        }

        tft->setFont(&FreeSans9pt7b);
        tft->setTextColor(ST77XX_WHITE);
        tft->setCursor(10, 182);
        tft->print(detailLine);

        tft->setFont(&FreeSans12pt7b);
        tft->setTextColor(statusColor);
        tft->setCursor(10, 206);
        if(baudConnected) {
          const int iconX = 12;
          const int iconY = 198;
          tft->drawLine(iconX, iconY + 8, iconX + 5, iconY + 13, COLOR_GREEN);
          tft->drawLine(iconX + 5, iconY + 13, iconX + 16, iconY + 2, COLOR_GREEN);
          char okLine[24];
          snprintf(okLine, sizeof(okLine), "%lu", static_cast<unsigned long>(targetBaud));
          tft->setCursor(34, 206);
          tft->print(okLine);
        } else {
          tft->print(motor_get_jmc_baud_trial_status_text());
        }

        tft->setFont(&FreeSans9pt7b);
        tft->setTextColor(ST77XX_WHITE);
        tft->setCursor(10, 228);
        tft->print(baudLine);

        tft->setCursor(10, 248);
        tft->print(readLine);

        prevTrialStatus = trialStatus;
        prevTrialBaud = trialBaud;
        prevReadBeforeBaud = readBeforeBaud;
        prevReadAfterBaud = readAfterBaud;
        prevTargetBaud = targetBaud;
        prevBaudConnected = baudConnected;
        prevUartLabel = uartLabel;
        prevDriveLabel = driveLabel;
      }
      prevValue = item.value;
    }
    else {
      if(prevValue != item.value) {
        tft->fillRect(0, 60, SCREEN_WIDTH, SCREEN_HEIGHT-60, ST77XX_BLACK);
        prevValue = item.value;
        if(menuNum == 1 && selectedIdx == Menu1Items::RADIOMODE) {
            const int selected = item.value < 0 ? 0 : (item.value > 3 ? 3 : item.value);
            for(int i = 0; i < 4; ++i) {
                const bool active = i == selected;
                const GFXfont* font = active ? &FreeSans18pt7b : &FreeSans12pt7b;
                const char* label = radio_mode_value_label(i);
                tft->setFont(font);
                tft->setTextColor(active ? COLOR_GREEN : ST77XX_WHITE);

                int16_t x1, y1;
                uint16_t w, h;
                tft->getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
                const int y = 86 + i * 36;
                tft->setCursor((SCREEN_WIDTH - w) / 2, y);
                tft->print(label);
                if(active) {
                    tft->drawRoundRect((SCREEN_WIDTH - w) / 2 - 8, y - h - 6, w + 22, h + 16, 6, COLOR_GREEN);
                }
            }

            tft->setFont(&FreeSans9pt7b);
            tft->setTextColor(ST77XX_WHITE);
            const char* msg1 = "OFF sleeps from idle.";
            const char* msg2 = "Wake with local button.";
            int16_t x1, y1;
            uint16_t w, h;
            tft->getTextBounds(msg1, 0, 0, &x1, &y1, &w, &h);
            tft->setCursor((SCREEN_WIDTH - w) / 2, 220);
            tft->print(msg1);
            tft->getTextBounds(msg2, 0, 0, &x1, &y1, &w, &h);
            tft->setCursor((SCREEN_WIDTH - w) / 2, 240);
            tft->print(msg2);
        // If min/max are 0/1, show "Disabled"/"Enabled" with box around selected
        } else if (item.minValue == 0 && item.maxValue == 1 && item.scalar == 1) {
            const char* labels[2] = {"Disabled", "Enabled"};
            const char* alt_labels[2] = {"Don't save", "Save"};
            const char* alt_label2[2] = {"Back", "RESET"};
            const char* directionLabels[2] = {"Normal", "Reversed"};
            int selected = (item.value == 0) ? 0 : 1;
            String macCopy = "", nameCopy = "";
            if (xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
              if(scale_mac != "") {
                macCopy = scale_mac;
                nameCopy = scale_name;
              } else { 
                macCopy = scale_connect_mac;
                nameCopy = scale_connect_name;
              }
              xSemaphoreGive(scaleMutex);
            } 

            for (int i = 0; i < 2; ++i) {
                const GFXfont* font = (i == selected) ? &FreeSans18pt7b : &FreeSans12pt7b;
                tft->setFont(font);
                tft->setTextColor(ST77XX_WHITE);
                int16_t x1, y1; uint16_t w, h;
                // Special case: Save scale (menu3.7)
                if(menuNum == 3 && selectedIdx == Menu3Items::SAVE_SCALE) {
                  tft->getTextBounds(alt_labels[i], 0, 0, &x1, &y1, &w, &h);

                  int y = 100 + i * 50;
                  tft->setCursor((SCREEN_WIDTH - w) / 2, y);
                  tft->print(alt_labels[i]);
                  if (i == selected) {
                      tft->drawRoundRect((SCREEN_WIDTH - w) / 2 - 8, y - h - 6 , w + 22, h + 16, 6, COLOR_GREEN);
                  }

                  if(i == 0) {
                    const char* msg4 = "last:";
                    tft->setFont(&FreeSans9pt7b);

                    int16_t x1, y1; uint16_t w, h;
                
                    tft->setCursor(10, 180);
                    tft->print(msg4);

                    tft->getTextBounds(msg4, 0, 0, &x1, &y1, &w, &h);
                    tft->setCursor(w + 30, 180);
                    tft->print(nameCopy);

                    tft->setFont(); // basic small font
                    tft->getTextBounds(macCopy, 0, 0, &x1, &y1, &w, &h);
                    tft->setCursor((SCREEN_WIDTH - w) / 2, 200);
                    tft->print(macCopy); 
                  } 
                } else if(menuNum == 1 && selectedIdx == Menu1Items::RADIOMODE) {
                  const char* radioLabels[2] = {"Web UI", "ESP-NOW"};
                  tft->getTextBounds(radioLabels[i], 0, 0, &x1, &y1, &w, &h);
                  int y = 100 + i * 50;
                  tft->setCursor((SCREEN_WIDTH - w) / 2, y);
                  tft->print(radioLabels[i]);
                  if (i == selected) {
                      tft->drawRoundRect((SCREEN_WIDTH - w) / 2 - 8, y - h - 6 , w + 22, h + 16, 6, COLOR_GREEN);
                  }

                  if(i == 0) {
                    tft->setFont(&FreeSans9pt7b);
                    const char* msg1 = "Web UI keeps AP + home Wi-Fi.";
                    const char* msg2 = "ESP-NOW disables the web server.";
                    tft->setCursor(12, 184);
                    tft->print(msg1);
                    tft->setCursor(12, 204);
                    tft->print(msg2);
                  }
                } else if(menuNum == 1 && (selectedIdx == Menu1Items::RESET || selectedIdx == Menu1Items::RESETNETWORK)) { 
                  tft->getTextBounds(alt_label2[i], 0, 0, &x1, &y1, &w, &h);
                  int y = 100 + i * 50;
                  tft->setCursor((SCREEN_WIDTH - w) / 2, y);
                  tft->print(alt_label2[i]);
                  if (i == selected) {
                      tft->drawRoundRect((SCREEN_WIDTH - w) / 2 - 8, y - h - 6 , w + 22, h + 16, 6, COLOR_GREEN);
                  }
                } else if(menuNum == 1 && selectedIdx == Menu1Items::SETMOTORDIRECTION) {
                  tft->getTextBounds(directionLabels[i], 0, 0, &x1, &y1, &w, &h);
                  int y = 100 + i * 50;
                  tft->setCursor((SCREEN_WIDTH - w) / 2, y);
                  tft->print(directionLabels[i]);
                  if (i == selected) {
                      tft->drawRoundRect((SCREEN_WIDTH - w) / 2 - 8, y - h - 6 , w + 22, h + 16, 6, COLOR_GREEN);
                  }

                  if(i == 0) {
                    tft->setFont(&FreeSans9pt7b);
                    const char* msg1 = "Normal keeps stock grind polarity.";
                    const char* msg2 = "Reversed flips JMC, RT, and KRS.";
                    tft->setCursor(10, 184);
                    tft->print(msg1);
                    tft->setCursor(10, 204);
                    tft->print(msg2);
                  }
                } else 
                { // normal case
                  tft->getTextBounds(labels[i], 0, 0, &x1, &y1, &w, &h);

                  int y = 100 + i * 50;
                  tft->setCursor((SCREEN_WIDTH - w) / 2, y);
                  tft->print(labels[i]);
                  if (i == selected) {
                      tft->drawRoundRect((SCREEN_WIDTH - w) / 2 - 8, y - h - 6 , w + 22, h + 16, 6, COLOR_GREEN);
                  }
                }
            }

        } else {
          // DRAW VALUE + UNIT
          char valStr[16];
          snprintf(valStr, sizeof(valStr), "%d", item.value);

          const GFXfont* fontVal  = &FreeSans24pt7b;
          const GFXfont* fontUnit = &FreeSans12pt7b;  // pick your unit font

          // Measure value
          tft->setFont(fontVal);
          int16_t x1v, y1v; uint16_t wv, hv;
          tft->getTextBounds(valStr, 0, 0, &x1v, &y1v, &wv, &hv);

          // Measure a space (for padding between value and unit) using the unit font
          tft->setFont(fontUnit);
          int16_t x1s, y1s; uint16_t ws, hs;
          tft->getTextBounds("   ", 0, 0, &x1s, &y1s, &ws, &hs);

          // Measure unit
          int16_t x1u, y1u; uint16_t wu, hu;
          tft->getTextBounds(item.unit ? item.unit : "", 0, 0, &x1u, &y1u, &wu, &hu);

          // Total width to center
          uint16_t totalW = wv + ws + wu;
          int16_t left = (SCREEN_WIDTH - totalW) / 2;

          // Choose baseline (same for both so they align)
          int16_t yBase = SCREEN_HEIGHT / 2;

          tft->setTextColor(COLOR_GREEN);

          // Draw value
          tft->setFont(fontVal);
          tft->setCursor(left - x1v, yBase);   // compensate for x1v
          tft->print(valStr);

          tft->setTextColor(ST77XX_WHITE);

          // Draw unit
          tft->setFont(fontUnit);
          tft->setCursor(left + wv + ws - x1u, yBase);  // after value + space, compensate x1u
          tft->print(item.unit ? item.unit : "");

        }
      }
    }
} 
