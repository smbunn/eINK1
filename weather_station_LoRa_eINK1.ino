/* Heltec Automation weather_station example

   Modified by Simon Bunn - deep sleep version
   - Deep sleeps for ~2 minutes, wakes to update the time (one display refresh per wake)
   - Every 30 minutes: WiFi + Open-Meteo fetch, full redraw, LoRaWAN uplink
   - Forecast is cached in RTC memory so the whole screen (icons, 5-day strip,
     nav bar, battery) is redrawn on every wake - not just the time digits

   v3 changes:
   - Clock digits moved 10 px left
   - "LoRa" indicator in the nav bar: plain text = joined/sending OK,
     struck through = no LoRa connection
*/

#include "LoRaWan_APP.h"
#include "GXHTC.h"
#include "board-config.h"
#include "secrets.h"
#include "HT_E0213A367.h"
#include "images.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include "time.h"
#include <HTTPClient.h>
#include "esp_task_wdt.h"
#include "esp_sleep.h"

/********************************************************
 *              Forecast Widget Support                 *
 ********************************************************/
struct DayForecast {
  char label[12];        // "Today Mon" / "Tue"
  uint8_t weatherCode;
  int tMin;
  int tMax;
};

// ---------------------- Watchdog / robustness ----------------------
static const int WDT_TIMEOUT_SECONDS = 120;
static inline void wdtKick() { esp_task_wdt_reset(); }

// ---------------------- Time / WiFi ----------------------
const char* ntpServer = "pool.ntp.org";

const char* tzInfo = "NZST-12NZDT,M9.5.0,M4.1.0/3";
const char* location = "Auckland";
// ===== TIMING =====
static const uint32_t TIME_UPDATE_SEC    = 2UL * 60UL;    // wake every 2 minutes
static const uint32_t WEATHER_PERIOD_SEC = 30UL * 60UL;   // weather + LoRa every 30 minutes
static const uint32_t LORA_JOIN_TIMEOUT_MS    = 5UL * 60UL * 1000UL;  // OTAA join can take minutes
static const uint32_t LORA_SESSION_TIMEOUT_MS = 110000;   // send phase, after join
static const uint32_t LORA_POST_SEND_GRACE_MS = 30000;    // RX windows + confirmed retries
static const uint8_t  LORA_MAX_FAILED_TRIES   = 3;        // then wait for next 30-min cycle
static const time_t   MIN_VALID_EPOCH = 1600000000;       // sanity check for RTC clock

// ===== STATE PRESERVED ACROSS DEEP SLEEP (RTC memory) =====
RTC_DATA_ATTR bool     rtcWeatherValid = false;
RTC_DATA_ATTR time_t   rtcLastWeatherEpoch = 0;
RTC_DATA_ATTR uint8_t  rtcCodes[6];          // today + next 5 days
RTC_DATA_ATTR int8_t   rtcTmin[6];
RTC_DATA_ATTR int8_t   rtcTmax[6];
RTC_DATA_ATTR float    tmin_today = NAN;     // floats sent over LoRa
RTC_DATA_ATTR float    tmax_today = NAN;
RTC_DATA_ATTR bool     shouldSendLoRa = false;
RTC_DATA_ATTR uint8_t  loraFailCount = 0;
RTC_DATA_ATTR bool     rtcLoraOk = false;    // true once joined + sending OK
RTC_DATA_ATTR uint32_t bootCount = 0;

// Initialize the display
HT_E0213A367 display(3, 2, 5, 1, 4, 6, -1, 6000000);  // rst,dc,cs,busy,sck,mosi,miso,frequency
struct tm timeinfo;

#define DIRECTION ANGLE_0_DEGREE

// ==== OPEN-METEO SETTINGS ====
// Trimmed: no "hourly" block (it was never used and made the JSON too big to parse)
const char* openMeteoUrl =
  "http://api.open-meteo.com/v1/forecast"
  "?latitude=-36.8509"
  "&longitude=174.7645"
  "&daily=temperature_2m_max,temperature_2m_min,weather_code"
  "&current=temperature_2m,relative_humidity_2m,weather_code"
  "&forecast_days=7"
  "&timezone=Pacific%2FAuckland";

/********************************************************
 *                  LoRa Settings                       *
 ********************************************************/

uint8_t nwkSKey[16];
uint8_t appSKey[16];
uint32_t devAddr;

uint16_t userChannelsMask[6] = { 0xFF00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 };

LoRaMacRegion_t loraWanRegion = ACTIVE_REGION;
DeviceClass_t loraWanClass = CLASS_A;

uint32_t appTxDutyCycle = TIME_UPDATE_SEC * 1000UL;  // required by library
bool overTheAirActivation = true;
bool loraWanAdr = true;
bool isTxConfirmed = true;
uint8_t appPort = 2;

uint8_t confirmedNbTrials = 4;

// Per-boot (not RTC) LoRa session bookkeeping
static uint32_t loraStartMs = 0;
static uint32_t sendDoneMs = 0;
static bool     sendIssued = false;

const char* weekdayShortName(int wday) {
  static const char* names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  if (wday < 0 || wday > 6) return "?";
  return names[wday];
}

/*************************************************
*               eINK Display Power               *
**************************************************/
void VextON(void) {
  pinMode(18, OUTPUT);
  digitalWrite(18, HIGH);
}
void VextOFF(void) {
  pinMode(18, OUTPUT);
  digitalWrite(18, LOW);
}

/*************************************************************************
*              Draw Weather Icons (stored in images.h)                   *
**************************************************************************/
void drawWeatherIcon(uint8_t wmoCode, int x, int y) {
  const unsigned char* iconBits = nullptr;
  int w = 48;
  int h = 48;

  switch (wmoCode) {
    case 0: iconBits = icon_sunny_code_0; break;
    case 1:
    case 2: iconBits = icon_overcast_code_1_2; break;
    case 3: iconBits = icon_cloudy_code_3; break;

    case 45:
    case 48:
      iconBits = icon_cloudy_code_3; break;

    case 51: iconBits = icon_drizzle_code_51; break;
    case 53:
    case 55: iconBits = icon_med_drizzle_code_53_55; break;
    case 56:
    case 57: iconBits = icon_freezing_drizzle_code_56_57; break;

    case 61: iconBits = icon_rain_code_61; break;
    case 63: iconBits = icon_rain_code_63; break;
    case 65: iconBits = icon_rain_code_65; break;

    case 66:
    case 67: iconBits = icon_freezing_rain_code_66_67; break;

    case 71:
    case 72:
    case 75:
    case 85:
    case 86: iconBits = icon_snow_code_71_72_75_85_86; break;

    case 77: iconBits = icon_snow_grains_code_77; break;

    case 80: iconBits = icon_light_rain_code_80; break;
    case 81: iconBits = icon_med_rain_code_81; break;
    case 82: iconBits = icon_heavy_rain_code_82; break;

    case 95: iconBits = icon_thunder_code_95; break;
    case 96:
    case 99: iconBits = icon_thunder_code_96_99; break;

    default: iconBits = icon_cloudy_code_3; break;
  }

  if (iconBits != nullptr) {
    display.drawXbm(x, y, w, h, iconBits);
  }
}

/*********************************************************
*                Emojis for Serial printout              *
**********************************************************/
String getWeatherEmoji(uint8_t code) {
  switch (code) {
    case 0: return "☀️";
    case 1: return "🌤️";
    case 2: return "⛅";
    case 3: return "☁️";
    case 45:
    case 48: return "🌫️";
    case 51:
    case 53:
    case 55: return "🌦️";
    case 56:
    case 57: return "🌧️❄️";
    case 61:
    case 63:
    case 65: return "🌧️";
    case 66:
    case 67: return "🌧️❄️";
    case 71:
    case 73:
    case 75:
    case 77: return "🌨️";
    case 80:
    case 81:
    case 82: return "🌦️";
    case 85:
    case 86: return "🌨️";
    case 95:
    case 96:
    case 99: return "⛈️";
    default: return "❓";
  }
}

/*****************************************************
 *              Battery Level Display                *
 *****************************************************/
// --- Battery measurement (Vision Master E213, verified by sweep) ---
#define VBAT_READ   7         // ADC1_CH6 — battery sense via 4.9:1 divider
#define ADC_CTRL    46        // HIGH = divider connected, LOW = disconnected

float readBatteryVolts() {
  pinMode(ADC_CTRL, OUTPUT);
  digitalWrite(ADC_CTRL, HIGH);     // enable divider
  delay(20);

  analogReadResolution(12);
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(VBAT_READ);

  digitalWrite(ADC_CTRL, LOW);      // disconnect to save power in deep sleep
  return (mv / 16) / 1000.0f * 4.9f;
}

void battery() {
  float vbat = readBatteryVolts();
  Serial.printf("Battery voltage = %.2f V\n", vbat);

  if (vbat < 3.0f) {
    display.drawString(230, 0, "N/A");                              // no battery / USB only
    display.drawXbm(230, 0, battery_w, battery_h, battery0);
  } else if (vbat < 3.45f) {
    display.drawXbm(230, 0, battery_w, battery_h, battery0);        // ~0–5%
  } else if (vbat < 3.60f) {
    display.drawXbm(230, 0, battery_w, battery_h, battery1);        // ~5–20%
  } else if (vbat < 3.70f) {
    display.drawXbm(230, 0, battery_w, battery_h, battery2);        // ~20–40%
  } else if (vbat < 3.80f) {
    display.drawXbm(230, 0, battery_w, battery_h, battery3);        // ~40–55%
  } else if (vbat < 3.90f) {
    display.drawXbm(230, 0, battery_w, battery_h, battery4);        // ~55–70%
  } else if (vbat < 4.00f) {
    display.drawXbm(230, 0, battery_w, battery_h, battery5);        // ~70–85%
  } else if (vbat < 4.10f) {
    display.drawXbm(230, 0, battery_w, battery_h, battery6);        // ~85–95%
  } else {
    display.drawXbm(230, 0, battery_w, battery_h, batteryfull);     // ~95–100%
  }
}

/**********************************************
*              Navigation Bar                 *
***********************************************/
void Navigation_bar() {
  display.setFont(ArialMT_Plain_10);
  display.drawLine(0, 15, 250, 15);
  display.drawXbm(5, -3, 20, 20, wifix_bitfis);

  display.drawString(35, 0, ssid);

  struct tm nowTm;
  char header[40];
  if (getLocalTime(&nowTm)) {
    char dateBuf[12];
    strftime(dateBuf, sizeof(dateBuf), "%d %b", &nowTm);
    snprintf(header, sizeof(header), "%s %s", location, dateBuf);
  } else {
    snprintf(header, sizeof(header), "%s", location);
  }

  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(125, 0, header);

  display.setTextAlignment(TEXT_ALIGN_LEFT);

  // LoRa status indicator: "LoRa" = joined/sending OK, struck through = not
  display.drawString(188, 0, "LoRa");
  if (!rtcLoraOk) {
    display.drawLine(186, 7, 214, 7);   // strikethrough = no LoRa connection
  }

  battery();
}

// ---------------------- Clock drawing (buffer only, no refresh) ----------------------
void draw_colon() {
  display.drawXbm(183, 28, 3, 18, colon3x18);
}

const unsigned char* numBitmap(uint8_t num) {
  switch (num) {
    case 0: return num0;
    case 1: return num1;
    case 2: return num2;
    case 3: return num3;
    case 4: return num4;
    case 5: return num5;
    case 6: return num6;
    case 7: return num7;
    case 8: return num8;
    case 9: return num9;
    default: return num0;
  }
}

// Draw HH:MM digits into the frame buffer. Caller does the single refresh.
void drawTimeDigits() {
  if (!getLocalTime(&timeinfo)) return;
  // Shifted 10 px left of the original layout
  display.dis_img_Partial_Refresh(145, 20, 32, 14, numBitmap(timeinfo.tm_hour / 10), true, true, false);
  display.dis_img_Partial_Refresh(160, 20, 32, 14, numBitmap(timeinfo.tm_hour % 10), true, true, false);
  display.dis_img_Partial_Refresh(195, 20, 32, 14, numBitmap(timeinfo.tm_min / 10), true, true, false);
  display.dis_img_Partial_Refresh(210, 20, 32, 14, numBitmap(timeinfo.tm_min % 10), true, true, false);
}

/***********************************************************************
*                  Next Row down, Today's Weather                      *
************************************************************************/
void drawTodayBlock(const DayForecast& today, int yStart) {
  const int ICON_W = 48;

  display.drawLine(0, 53, 250, 53);
  display.drawLine(125, 15, 125, 53);

  int iconX = 2;
  int iconY = yStart;
  drawWeatherIcon(today.weatherCode, iconX, iconY);

  int textX = iconX + ICON_W + 2;
  int textY = iconY + 5;

  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(textX, textY, today.label);

  char tempLine[20];
  snprintf(tempLine, sizeof(tempLine), "%d / %d°C", today.tMin, today.tMax);
  display.drawString(textX, textY + 14, tempLine);
  display.drawXbm(textX + 56, textY + 4, 10, 17, temp);
}

/*********************************************************************
*      Strip of up to 5 days with day label, icon and max temp       *
**********************************************************************/
void drawFiveDayStrip(DayForecast days[], int count, int yStart) {
  const int SCREEN_W = 250;
  const int ICON_W = 48;
  const int ICON_H = 48;

  if (count <= 0) return;
  if (count > 5) count = 5;

  int colWidth = SCREEN_W / count;

  for (int i = 0; i < count; i++) {
    int colXCenter = colWidth * i + colWidth / 2;

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(colXCenter, yStart + 1, days[i].label);

    int iconX = colXCenter - ICON_W / 2;
    int iconY = yStart + 4;
    drawWeatherIcon(days[i].weatherCode, iconX, iconY);

    char tStr[8];
    snprintf(tStr, sizeof(tStr), "%d°", days[i].tMax);
    display.drawString(colXCenter, iconY + ICON_H, tStr);
  }
}

/***********************************************************************
*       Build DayForecast structs from the RTC-cached forecast         *
************************************************************************/
void buildForecastsFromRTC(DayForecast& today, DayForecast next5[5]) {
  int baseWday = -1;
  if (getLocalTime(&timeinfo)) baseWday = timeinfo.tm_wday;

  today.weatherCode = rtcCodes[0];
  today.tMin = rtcTmin[0];
  today.tMax = rtcTmax[0];
  if (baseWday >= 0) {
    snprintf(today.label, sizeof(today.label), "Today %s", weekdayShortName(baseWday));
  } else {
    snprintf(today.label, sizeof(today.label), "Today");
  }

  for (int i = 0; i < 5; i++) {
    next5[i].weatherCode = rtcCodes[i + 1];
    next5[i].tMin = rtcTmin[i + 1];
    next5[i].tMax = rtcTmax[i + 1];
    if (baseWday >= 0) {
      snprintf(next5[i].label, sizeof(next5[i].label), "%s", weekdayShortName((baseWday + i + 1) % 7));
    } else {
      snprintf(next5[i].label, sizeof(next5[i].label), "Day%d", i + 1);
    }
  }
}

/***********************************************************************
*   Redraw the ENTIRE screen (nav bar, weather, clock) - ONE refresh   *
************************************************************************/
void redrawScreen() {
  display.clear();
  Navigation_bar();

  if (rtcWeatherValid) {
    DayForecast today, next5[5];
    buildForecastsFromRTC(today, next5);
    int todayY = 14;
    drawTodayBlock(today, todayY);
    drawFiveDayStrip(next5, 5, todayY + 40);
  } else {
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(5, 60, "No weather data yet");
  }

  drawTimeDigits();
  draw_colon();

  display.update(COLOR_BUFFER);
  display.display();
}

/***********************************************************************
*             MAIN WEATHER DATA COLLECT (stores to RTC)                *
************************************************************************/
bool fetchWeather() {
  Serial.println("\n=== Fetching weather from Open-Meteo ===");
  wdtKick();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return false;
  }

  HTTPClient http;
  WiFiClient client;
  http.setTimeout(15000);

  if (!http.begin(client, openMeteoUrl)) {
    Serial.println("HTTP begin() failed");
    return false;
  }

  wdtKick();
  int httpCode = http.GET();

  if (httpCode <= 0) {
    Serial.print("HTTP GET failed, error: ");
    Serial.println(http.errorToString(httpCode));
    http.end();
    return false;
  }

  Serial.print("HTTP code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  wdtKick();
  String payload = http.getString();
  http.end();

  Serial.print("Payload length: ");
  Serial.println(payload.length());

  // Heap-allocated (NOT on the stack) - the old StaticJsonDocument<32768>
  // overflowed the task stack and was too small for the response anyway.
  DynamicJsonDocument doc(12288);

  wdtKick();
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return false;
  }

  wdtKick();

  // ===== CURRENT DATA (serial log only) =====
  JsonObject current = doc["current"];
  float current_temp = current["temperature_2m"] | NAN;
  float current_rh = current["relative_humidity_2m"] | NAN;
  int current_weather_code = current["weather_code"] | -1;

  Serial.println("=== Current Data ===");
  Serial.print("Temp (°C): ");    Serial.println(current_temp);
  Serial.print("RH (%): ");       Serial.println(current_rh);
  Serial.print("Weather code: "); Serial.println(current_weather_code);

  // ===== DAILY DATA -> RTC cache =====
  JsonObject daily = doc["daily"];
  JsonArray daily_temp_max = daily["temperature_2m_max"];
  JsonArray daily_temp_min = daily["temperature_2m_min"];
  JsonArray daily_weather_code = daily["weather_code"];

  if (daily_temp_max.size() < 6 || daily_temp_min.size() < 6 || daily_weather_code.size() < 6) {
    Serial.println("Not enough daily forecast data.");
    return false;
  }

  for (int i = 0; i < 6; i++) {
    rtcCodes[i] = (uint8_t)(int)daily_weather_code[i];
    rtcTmin[i] = (int8_t)lround((float)daily_temp_min[i]);
    rtcTmax[i] = (int8_t)lround((float)daily_temp_max[i]);
    Serial.printf("Day %d: code %d %s, %d / %d C\n",
                  i, rtcCodes[i], getWeatherEmoji(rtcCodes[i]).c_str(), rtcTmin[i], rtcTmax[i]);
  }

  tmin_today = (float)daily_temp_min[0];
  tmax_today = (float)daily_temp_max[0];
  rtcWeatherValid = true;
  return true;
}

/***********************************************************************
*                  WiFi + Real Time Clock                              *
************************************************************************/
bool connectWiFi() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to ");
  Serial.println(ssid);

  const uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    wdtKick();
    delay(250);
    Serial.print(".");
    if (millis() - wifiStart > 30000) {
      Serial.println("\nWiFi connect timeout");
      return false;
    }
  }
  Serial.println("\nWiFi connected");
  Serial.println(WiFi.localIP());
  return true;
}

void GetNetTime() {
  configTzTime(tzInfo, ntpServer);
  // Wait up to 10 s for NTP sync
  getLocalTime(&timeinfo, 10000);
}

/***********************************************************************
*                  Prepare data payload for LoRa                       *
************************************************************************/
static void prepareTxFrame(uint8_t port) {
  unsigned char* puc;
  appDataSize = 0;

  appData[appDataSize++] = 0x04;
  appData[appDataSize++] = 0x00;
  appData[appDataSize++] = 0x0A;
  appData[appDataSize++] = 0x02;

  puc = (unsigned char*)(&tmin_today);
  appData[appDataSize++] = puc[0];
  appData[appDataSize++] = puc[1];
  appData[appDataSize++] = puc[2];
  appData[appDataSize++] = puc[3];

  appData[appDataSize++] = 0x12;

  puc = (unsigned char*)(&tmax_today);
  appData[appDataSize++] = puc[0];
  appData[appDataSize++] = puc[1];
  appData[appDataSize++] = puc[2];
  appData[appDataSize++] = puc[3];
}

/***********************************************************************
*                 Downlink data handler for LoRa                       *
************************************************************************/
void downLinkDataHandle(McpsIndication_t* mcpsIndication) {
  Serial.printf("+REV DATA:%s,RXSIZE %d,PORT %d\r\n",
                mcpsIndication->RxSlot ? "RXWIN2" : "RXWIN1",
                mcpsIndication->BufferSize,
                mcpsIndication->Port);

  Serial.print("+REV DATA:");
  for (uint8_t i = 0; i < mcpsIndication->BufferSize; i++) {
    Serial.printf("%02X", mcpsIndication->Buffer[i]);
  }
  Serial.println();
}

/***********************************************************************
*                        DEEP SLEEP                                    *
************************************************************************/
void goToSleep() {
  // Align the next wake to just after a 2-minute clock boundary so the
  // displayed time is accurate when we wake.
  uint32_t sleepSec = TIME_UPDATE_SEC;
  time_t now = time(nullptr);
  if (now > MIN_VALID_EPOCH) {
    uint32_t into = (uint32_t)(now % TIME_UPDATE_SEC);
    sleepSec = TIME_UPDATE_SEC - into + 2;     // wake ~2 s after the boundary
    if (sleepSec < 20) sleepSec += TIME_UPDATE_SEC;
  }

  Serial.printf("Deep sleeping for %u s (boot #%u)\n", sleepSec, bootCount);
  Serial.flush();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  VextOFF();  // cut display power - e-ink keeps its image with no power

  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  esp_deep_sleep_start();
}

/***********************************************************************
*                               SETUP                                  *
************************************************************************/
void setup() {
  Serial.begin(115200);
  bootCount++;

  // (Re)configure Task Watchdog
  esp_task_wdt_deinit();
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
    .idle_core_mask = (1 << 0) | (1 << 1),
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  bool coldBoot = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED);
  Serial.printf("\nWake (boot #%u, %s)\n", bootCount, coldBoot ? "cold boot" : "timer");

  // The RTC keeps the clock through deep sleep, but the TZ environment
  // variable does not survive - restore it every boot (no network needed).
  setenv("TZ", tzInfo, 1);
  tzset();

  VextON();
  delay(100);
  display.init();
  display.screenRotate(DIRECTION);

  time_t now = time(nullptr);
  bool timeValid = now > MIN_VALID_EPOCH;
  bool weatherDue = coldBoot || !rtcWeatherValid || !timeValid ||
                    (now - rtcLastWeatherEpoch >= (time_t)(WEATHER_PERIOD_SEC - 30));

  if (weatherDue) {
    Serial.println("*** 30-minute cycle: WiFi + weather + LoRa ***");
    if (coldBoot) {
      display.clear();
      display.setFont(ArialMT_Plain_10);
      display.drawString(0, 0, "Connecting to WiFi:");
      display.drawString(0, 20, ssid);
      display.update(COLOR_BUFFER);
      display.display();
    }
    if (connectWiFi()) {
      GetNetTime();
      wdtKick();
      if (fetchWeather()) {
        rtcLastWeatherEpoch = time(nullptr);
        shouldSendLoRa = true;   // survives deep sleep until the send succeeds
        loraFailCount = 0;       // fresh data, fresh attempts
      }
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  } else {
    Serial.println("*** 2-minute wake: time update only ***");
  }

  // Full redraw from RTC cache + current time - ONE display refresh
  redrawScreen();

  if (!shouldSendLoRa) {
    // Nothing to transmit - straight back to sleep. loop() is never reached.
    goToSleep();
  }

  // LoRa uplink path (every 30 minutes, or retrying a previously failed send)
  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);
  deviceState = DEVICE_STATE_INIT;
  loraStartMs = millis();
  Serial.println("Setup complete - running LoRa state machine");
}

/**********************************************************************
*                               LOOP                                  *
*   Only reached on send cycles; ends in deep sleep either way.       *
***********************************************************************/
void loop() {
  wdtKick();

  // Safety net: never stay awake past the limit. Joining gets longer
  // (TTN join retries are slow); the send phase gets a shorter window.
  uint32_t limitMs = (deviceState == DEVICE_STATE_INIT || deviceState == DEVICE_STATE_JOIN)
                       ? LORA_JOIN_TIMEOUT_MS : LORA_SESSION_TIMEOUT_MS;
  if (millis() - loraStartMs > limitMs) {
    loraFailCount++;
    rtcLoraOk = false;          // shown struck through on the display
    Serial.printf("LoRa session timeout (attempt %u) - sleeping\n", loraFailCount);
    if (loraFailCount >= LORA_MAX_FAILED_TRIES) {
      Serial.println("Giving up until next 30-minute cycle");
      shouldSendLoRa = false;
      loraFailCount = 0;
    }
    goToSleep();
  }

  switch (deviceState) {
    case DEVICE_STATE_INIT: {
      Serial.println("LoRaWAN Init");
      LoRaWAN.init(loraWanClass, loraWanRegion);
      LoRaWAN.setDefaultDR(3);
      break;
    }

    case DEVICE_STATE_JOIN: {
      wdtKick();
      LoRaWAN.join();
      wdtKick();
      break;
    }

    case DEVICE_STATE_SEND: {
      loraStartMs = millis();  // joined - restart the clock for the send phase
      if (shouldSendLoRa) {
        wdtKick();
        prepareTxFrame(appPort);
        LoRaWAN.send();
        wdtKick();
        Serial.println("LoRaWAN Send (weather data transmitted)");
        shouldSendLoRa = false;
        loraFailCount = 0;
        rtcLoraOk = true;       // joined and uplink sent OK

        // Refresh the on-screen indicator immediately so the current
        // LoRa session is reflected on the display, not only on the next wake.
        redrawScreen();
      }
      sendIssued = true;
      sendDoneMs = millis();
      deviceState = DEVICE_STATE_CYCLE;
      break;
    }

    case DEVICE_STATE_CYCLE: {
      txDutyCycleTime = appTxDutyCycle + randr(-APP_TX_DUTYCYCLE_RND, APP_TX_DUTYCYCLE_RND);
      LoRaWAN.cycle(txDutyCycleTime);
      deviceState = DEVICE_STATE_SLEEP;
      break;
    }

    case DEVICE_STATE_SLEEP: {
      // Let the stack finish RX windows / confirmed retries, then deep sleep.
      LoRaWAN.sleep(loraWanClass);
      if (sendIssued && (millis() - sendDoneMs > LORA_POST_SEND_GRACE_MS)) {
        goToSleep();
      }
      break;
    }

    default: {
      deviceState = DEVICE_STATE_INIT;
      break;
    }
  }

  delay(10);
}
