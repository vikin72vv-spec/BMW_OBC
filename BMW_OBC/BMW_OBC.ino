/*
 * BMW E34 OBC: V40 (FIXED WDT FOR ESP32 V3.0+)
 * 1. FIX: Исправлена инициализация Watchdog для новых версий ядра ESP32 (v3.0+).
 * 2. SYSTEM: Добавлена структура конфигурации таймера (esp_task_wdt_config_t).
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <RTClib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <esp_task_wdt.h>  // Библиотека сторожевого таймера

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "logo.h"
#include <pgmspace.h>

// ================= НАСТРОЙКИ =================
const char* wifi_ssid = "arduino";
const char* wifi_pass = "12345678";

// --- ССЫЛКА НА ПРОШИВКУ (ЗАМЕНИ НА СВОЮ RAW ССЫЛКУ С GITHUB!) ---
const char* FIRMWARE_URL = "https://github.com/vikin72vv-spec/BMW_OBC/blob/main/BMW_OBC.bin";

const char* CITY_NAME = "Mineralnye Vody";
const float LATITUDE = 44.21;
const float LONGITUDE = 43.13;

// WATCHDOG (Время в секундах до перезагрузки при зависании)
#define WDT_TIMEOUT 8

// ПИНЫ
#define PIN_SCLK 12
#define PIN_MOSI 11
#define PIN_MISO 40
#define PIN_DC 38
#define PIN_CS 10
#define PIN_RST 39
#define PIN_BLK 42
#define MP3_RX 16
#define MP3_TX 17
#define PIN_MIC 3
#define PIN_BUTTON 15
#define PIN_DS18B20 21
#define PIN_VOLT 4
#define PIN_RPM 5
#define PIN_SPEED 34
#define PIN_SDA 1
#define PIN_SCL 2

// ЦВЕТА
#define BMW_AMBER 0xFD20
#define COLOR_ALERT 0xF800
#define COLOR_OK 0x07E0
#define COLOR_COLD 0x001F
#define COLOR_BG 0x0000
#define COLOR_GAUGE_BG 0x2104
#define COLOR_FRAME 0x528A
#define COLOR_DARK_GREY 0x1082

#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF
#define TFT_GREY 0x8410
#define TFT_YELLOW 0xFFE0
#define TFT_BLUE 0x001F
#define STATUS_BG 0x18E3

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;
public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.freq_write = 40000000;
      cfg.pin_sclk = PIN_SCLK;
      cfg.pin_mosi = PIN_MOSI;
      cfg.pin_miso = PIN_MISO;
      cfg.pin_dc = PIN_DC;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = PIN_CS;
      cfg.pin_rst = PIN_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = false;
      cfg.rgb_order = true;
      cfg.bus_shared = true;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = PIN_BLK;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

LGFX tft;
HardwareSerial myMP3(2);
RTC_DS3231 rtc;
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);
Preferences pref;
WebServer server(80);

volatile int rpm_pulses = 0;
volatile int speed_pulses = 0;
unsigned long timer_update = 0;
int rpm = 0;
int speed_kmh = 0;
float volt = 0.0;
float tempC = -127;
unsigned long engine_total_seconds = 0;
#define SERVICE_LIMIT_HOURS 300

int current_bri = 255;
bool auto_bri = false;
int pulses_per_km = 4712;
int corr_temp = 0;
int corr_rpm = 0;
float corr_volt = 0.0;
int display_mode = 0;
#define MAX_MODES 8

#define BUF_W 280
int hist_temp[BUF_W];
int hist_volt[BUF_W];
int hist_rpm[BUF_W];
int buf_idx = 0;
String weatherDesc = "Loading...";
int weatherCode = 0;
float tempAir = 0.0;
bool weatherUpdated = false;

// HTML СТРАНИЦА
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><title>BMW E34 OBC</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>body{font-family:Arial;background:#111;color:#eee;text-align:center}.btn{background:#e67e22;color:white;padding:15px;margin:5px;border:none;width:90%;font-size:18px;border-radius:5px}.btn-red{background:#c0392b}.btn-blue{background:#2980b9}.btn-green{background:#27ae60}input{padding:10px;width:70px;text-align:center;margin:5px}.box{border:1px solid #444;margin:10px;padding:10px;border-radius:5px}</style></head><body>
<h1>BMW E34 CONTROL</h1>
<div class="box"><h3>MODE</h3><button class="btn" onclick="fetch('/set?m=0')">MAIN DASH</button><button class="btn" onclick="fetch('/set?m=1')">CLOCK</button><button class="btn" onclick="fetch('/set?m=5')">SERVICE</button><button class="btn" onclick="fetch('/set?m=7')">WEATHER</button><button class="btn" onclick="fetch('/set?m=6')">EQUALIZER</button></div>
<div class="box"><h3>DISPLAY</h3><form action="/set_bri">Bright: <input type="range" name="b" min="10" max="255" value="%BRI%" onchange="this.form.submit()"><br><label><input type="checkbox" name="a" value="1" %CHK% onchange="this.form.submit()"> Smart (Day/Night)</label></form></div>
<div class="box"><h3>CALIBRATION</h3><form action="/set_corr">Temp: <input name="t" value="%CT%"><br>RPM: <input name="r" value="%CR%"><br>Volt: <input name="v" value="%CV%"><br><button class="btn btn-blue">SAVE</button></form></div>
<div class="box"><h3>SYSTEM</h3><button class="btn btn-green" onclick="if(confirm('Update Firmware from GitHub?')) fetch('/ota')">GITHUB UPDATE</button><button class="btn btn-red" onclick="if(confirm('Reset Oil?')) fetch('/reset')">RESET OIL</button><button class="btn btn-red" onclick="fetch('/reboot')">REBOOT</button></div></body></html>)rawliteral";

void IRAM_ATTR rpm_interrupt() {
  rpm_pulses++;
}
void IRAM_ATTR speed_interrupt() {
  speed_pulses++;
}

void sendMP3Command(byte command, byte dat1, byte dat2) {
  byte buffer[10] = { 0x7E, 0xFF, 0x06, command, 0x00, dat1, dat2, 0x00, 0x00, 0xEF };
  uint16_t sum = 0;
  for (int i = 1; i < 7; i++) sum += buffer[i];
  sum = -sum;
  buffer[7] = (byte)(sum >> 8);
  buffer[8] = (byte)(sum);
  myMP3.write(buffer, 10);
}
void playTrack(int folder, int track) {
  sendMP3Command(0x0F, (byte)folder, (byte)track);
}
void setVolume(int vol) {
  sendMP3Command(0x06, 0, (byte)vol);
}

void updateWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    weatherDesc = "No WiFi";
    return;
  }
  WiFiClient client;
  HTTPClient http;
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(LATITUDE) + "&longitude=" + String(LONGITUDE) + "&current_weather=true";
  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);
    tempAir = doc["current_weather"]["temperature"];
    weatherCode = doc["current_weather"]["weathercode"];
    if (weatherCode == 0) weatherDesc = "Clear";
    else if (weatherCode < 3) weatherDesc = "Cloudy";
    else if (weatherCode < 60) weatherDesc = "Fog";
    else if (weatherCode < 80) weatherDesc = "Rain";
    else weatherDesc = "Snow";
    weatherUpdated = true;
  }
  http.end();
}

// === ФУНКЦИЯ ОБНОВЛЕНИЯ С GITHUB ===
void performOTA() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 50);
  tft.println("CONNECTING GITHUB...");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, FIRMWARE_URL);
  int httpCode = http.GET();

  if (httpCode == 200) {
    int contentLength = http.getSize();
    bool canBegin = Update.begin(contentLength);

    if (canBegin) {
      tft.println("DOWNLOADING...");
      WiFiClient* stream = http.getStreamPtr();
      size_t written = Update.writeStream(*stream);

      if (written == contentLength) {
        tft.println("WRITING...");
        if (Update.end()) {
          if (Update.isFinished()) {
            tft.setTextColor(COLOR_OK, TFT_BLACK);
            tft.println("UPDATE SUCCESS!");
            tft.println("REBOOTING...");
            delay(1000);
            ESP.restart();
          }
        } else {
          tft.setTextColor(COLOR_ALERT, TFT_BLACK);
          tft.println("UPDATE FAILED!");
        }
      } else {
        tft.setTextColor(COLOR_ALERT, TFT_BLACK);
        tft.println("WRITE ERROR!");
      }
    } else {
      tft.setTextColor(COLOR_ALERT, TFT_BLACK);
      tft.println("NOT ENOUGH SPACE");
    }
  } else {
    tft.setTextColor(COLOR_ALERT, TFT_BLACK);
    tft.print("HTTP ERROR: ");
    tft.println(httpCode);
  }
  http.end();
  delay(3000);
  drawScreen(true);
}

void drawHand(int x, int y, int length, int angle, int color, int width) {
  float rad = (angle - 90) * 0.0174532925;
  int x2 = x + cos(rad) * length;
  int y2 = y + sin(rad) * length;
  tft.drawLine(x, y, x2, y2, color);
  if (width > 1) {
    tft.drawLine(x + 1, y + 1, x2 + 1, y2 + 1, color);
    tft.drawLine(x - 1, y - 1, x2 - 1, y2 - 1, color);
  }
}

void drawTicks(int x, int y, int r, int start_angle, int end_angle, int num_ticks, uint16_t color, int len) {
  float step = (end_angle - start_angle) / (float)(num_ticks - 1);
  for (int i = 0; i < num_ticks; i++) {
    float angle = start_angle + i * step;
    float rad = (angle - 90) * 0.0174532925;
    int x1 = x + cos(rad) * (r - len);
    int y1 = y + sin(rad) * (r - len);
    int x2 = x + cos(rad) * r;
    int y2 = y + sin(rad) * r;
    tft.drawLine(x1, y1, x2, y2, color);
  }
}

void drawGauge(int x, int y, int r, int thick, float val, float min_val, float max_val, int start_angle, int end_angle, uint16_t color) {
  float range_val = max_val - min_val;
  float range_angle = end_angle - start_angle;
  float constrained_val = constrain(val, min_val, max_val);
  float angle_val = start_angle + ((constrained_val - min_val) / range_val) * range_angle;
  tft.fillArc(x, y, r, r - thick, angle_val, end_angle, COLOR_GAUGE_BG);
  tft.fillArc(x, y, r, r - thick, start_angle, angle_val, color);
  tft.drawArc(x, y, r, r, start_angle, end_angle, COLOR_FRAME);
  tft.drawArc(x, y, r - thick, r - thick, start_angle, end_angle, COLOR_FRAME);
  drawTicks(x, y, r + 5, start_angle, end_angle, 7, TFT_WHITE, 8);
}

void drawClock() {
  DateTime now = rtc.now();
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(130, 215);
  if (now.hour() < 10) tft.print('0');
  tft.print(now.hour());
  tft.print(':');
  if (now.minute() < 10) tft.print('0');
  tft.print(now.minute());
}

void drawWeatherIcon(int x, int y, int code) {
  if (code == 0) {
    tft.fillCircle(x, y, 30, TFT_YELLOW);
    for (int i = 0; i < 8; i++) {
      float ang = i * 45 * 0.0174;
      tft.drawLine(x + cos(ang) * 35, y + sin(ang) * 35, x + cos(ang) * 45, y + sin(ang) * 45, TFT_YELLOW);
    }
  } else if (code > 0 && code < 50) {
    tft.fillCircle(x - 20, y, 20, TFT_GREY);
    tft.fillCircle(x + 20, y + 5, 18, TFT_GREY);
    tft.fillCircle(x, y - 10, 25, TFT_WHITE);
  } else if ((code >= 50 && code < 70) || code >= 80) {
    tft.fillCircle(x - 20, y, 20, TFT_GREY);
    tft.fillCircle(x + 20, y + 5, 18, TFT_GREY);
    tft.fillCircle(x, y - 10, 25, TFT_WHITE);
    tft.drawLine(x - 10, y + 25, x - 15, y + 35, TFT_BLUE);
    tft.drawLine(x, y + 25, x - 5, y + 35, TFT_BLUE);
    tft.drawLine(x + 10, y + 25, x + 5, y + 35, TFT_BLUE);
  } else {
    tft.fillCircle(x - 20, y, 20, TFT_GREY);
    tft.fillCircle(x + 20, y + 5, 18, TFT_GREY);
    tft.fillCircle(x, y - 10, 25, TFT_WHITE);
    tft.fillCircle(x - 10, y + 30, 2, TFT_WHITE);
    tft.fillCircle(x + 10, y + 30, 2, TFT_WHITE);
    tft.fillCircle(x, y + 40, 2, TFT_WHITE);
  }
}

void drawStatusBar(bool full_redraw) {
  if (full_redraw) { tft.fillRect(0, 218, 320, 22, STATUS_BG); }
  int rssi = WiFi.RSSI();
  uint16_t wifiCol = COLOR_DARK_GREY;
  if (WiFi.status() == WL_CONNECTED) {
    if (rssi > -60) wifiCol = COLOR_OK;
    else if (rssi > -80) wifiCol = BMW_AMBER;
    else wifiCol = COLOR_ALERT;
  }
  tft.fillRect(5, 235, 3, 3, (WiFi.status() == WL_CONNECTED) ? wifiCol : COLOR_DARK_GREY);
  tft.fillRect(10, 232, 3, 6, (WiFi.status() == WL_CONNECTED && rssi > -90) ? wifiCol : COLOR_DARK_GREY);
  tft.fillRect(15, 229, 3, 9, (WiFi.status() == WL_CONNECTED && rssi > -80) ? wifiCol : COLOR_DARK_GREY);
  tft.fillRect(20, 226, 3, 12, (WiFi.status() == WL_CONNECTED && rssi > -60) ? wifiCol : COLOR_DARK_GREY);
  DateTime now = rtc.now();
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, STATUS_BG);
  tft.setCursor(130, 222);
  if (now.hour() < 10) tft.print('0');
  tft.print(now.hour());
  tft.print(':');
  if (now.minute() < 10) tft.print('0');
  tft.print(now.minute());
  tft.setCursor(250, 222);
  if (volt < 12.0) tft.setTextColor(COLOR_ALERT, STATUS_BG);
  else tft.setTextColor(BMW_AMBER, STATUS_BG);
  tft.print(volt, 1);
  tft.print("V");
}

void drawCenteredTitle(String title) {
  tft.setTextSize(2);
  tft.setTextColor(BMW_AMBER, TFT_BLACK);
  int w = title.length() * 12;
  int x = (320 - w) / 2;
  tft.setCursor(x, 8);
  tft.print(title);
}

void showStartupAnimation() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(BMW_AMBER, TFT_BLACK);
  int w = tft.textWidth("M POWER");
  tft.setCursor((320 - w) / 2, 100);
  tft.print("M POWER");
  for (int i = 0; i < 320; i += 10) {
    tft.fillRect(0, 140, i, 4, BMW_AMBER);
    delay(10);
  }
  delay(500);
  tft.fillScreen(TFT_BLACK);
  drawCenteredTitle("DIGITAL DASH");
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(55, 185);
  tft.print("TEMP");
  tft.setCursor(235, 185);
  tft.print("VOLT");
  for (int i = 0; i <= 100; i += 2) {
    float t_val = map(i, 0, 100, 0, 130);
    float v_val = map(i, 0, 100, 100, 160) / 10.0;
    drawGauge(80, 140, 70, 10, t_val, 0, 130, 135, 405, BMW_AMBER);
    drawGauge(240, 140, 70, 10, v_val, 10, 16, 135, 405, BMW_AMBER);
    delay(5);
  }
  for (int i = 100; i >= 0; i -= 2) {
    float t_val = map(i, 0, 100, 0, 130);
    float v_val = map(i, 0, 100, 100, 160) / 10.0;
    drawGauge(80, 140, 70, 10, t_val, 0, 130, 135, 405, BMW_AMBER);
    drawGauge(240, 140, 70, 10, v_val, 10, 16, 135, 405, BMW_AMBER);
    delay(5);
  }
  delay(200);
  tft.fillScreen(TFT_BLACK);
}

void drawScreen(bool redraw_static) {
  esp_task_wdt_reset();  // ПИНАЕМ СТОРОЖЕВОГО ПСА (СБРОС ТАЙМЕРА)

  if (display_mode == 0) {
    if (redraw_static) {
      tft.fillScreen(TFT_BLACK);
      drawCenteredTitle("DIGITAL DASH");
      tft.setTextSize(2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(55, 185);
      tft.print("TEMP");
      tft.setCursor(235, 185);
      tft.print("VOLT");
    }
    uint16_t t_col = COLOR_OK;
    if (tempC < 60) t_col = COLOR_COLD;
    else if (tempC > 105) t_col = COLOR_ALERT;
    drawGauge(80, 140, 70, 10, tempC, 0, 130, 135, 405, t_col);
    String t_str = String((int)tempC);
    tft.setFont(&Digital7_40pt7b);
    tft.setTextSize(1);
    tft.setTextColor(t_col, TFT_BLACK);
    int tw = tft.textWidth(t_str);
    tft.setCursor(80 - (tw / 2), 155);
    tft.print(t_str);
    tft.setFont(nullptr);
    uint16_t v_col = BMW_AMBER;
    if (volt < 12.0 || volt > 14.8) v_col = COLOR_ALERT;
    drawGauge(240, 140, 70, 10, volt, 10, 16, 135, 405, v_col);
    String v_str = String(volt, 1);
    tft.setFont(&Digital7_40pt7b);
    tft.setTextSize(1);
    tft.setTextColor(v_col, TFT_BLACK);
    int vw = tft.textWidth(v_str);
    tft.setCursor(240 - (vw / 2), 155);
    tft.print(v_str);
    tft.setFont(nullptr);
  } else if (display_mode == 1) {
    if (redraw_static) {
      tft.fillScreen(TFT_BLACK);
      tft.drawCircle(160, 115, 95, BMW_AMBER);
      tft.drawCircle(160, 115, 96, BMW_AMBER);
      drawTicks(160, 115, 95, 0, 360, 12, BMW_AMBER, 10);
      tft.setTextSize(2);
      tft.setTextColor(BMW_AMBER);
      for (int n = 1; n <= 12; n++) {
        float a = (n - 3) * 30 * 0.0174533;
        tft.setCursor(160 + cos(a) * 80 - 6, 115 + sin(a) * 80 - 8);
        tft.print(n);
      }
    }
    tft.fillCircle(160, 115, 70, TFT_BLACK);
    DateTime now = rtc.now();
    drawHand(160, 115, 40, now.hour() * 30 + now.minute() * 0.5, BMW_AMBER, 4);
    drawHand(160, 115, 60, now.minute() * 6, BMW_AMBER, 2);
    drawHand(160, 115, 65, now.second() * 6, COLOR_ALERT, 1);
    tft.fillCircle(160, 115, 4, BMW_AMBER);
  } else if (display_mode == 2) {
    if (redraw_static) {
      tft.fillScreen(TFT_BLACK);
      drawCenteredTitle("COOLANT TEMP");
    }
    drawGauge(160, 135, 90, 15, tempC, 0, 130, 135, 405, (tempC > 105) ? COLOR_ALERT : COLOR_OK);
    String t_str = String((int)tempC);
    tft.setFont(&Digital7_40pt7b);
    tft.setTextSize(1);
    tft.setTextColor(BMW_AMBER, TFT_BLACK);
    int tw = tft.textWidth(t_str);
    tft.setCursor(160 - (tw / 2), 155);
    tft.print(t_str);
    tft.setFont(nullptr);
  } else if (display_mode == 3) {
    if (redraw_static) {
      tft.fillScreen(TFT_BLACK);
      drawCenteredTitle("BATTERY VOLTAGE");
    }
    drawGauge(160, 135, 90, 15, volt, 10, 16, 135, 405, COLOR_OK);
    String v_str = String(volt, 1);
    tft.setFont(&Digital7_40pt7b);
    tft.setTextSize(1);
    tft.setTextColor(BMW_AMBER, TFT_BLACK);
    int vw = tft.textWidth(v_str);
    tft.setCursor(160 - (vw / 2), 155);
    tft.print(v_str);
    tft.setFont(nullptr);
  } else if (display_mode == 4) {
    if (redraw_static) {
      tft.fillScreen(TFT_BLACK);
      drawCenteredTitle("ENGINE SPEED");
    }
    drawGauge(160, 135, 90, 15, rpm, 0, 8000, 135, 405, (rpm > 6000) ? COLOR_ALERT : BMW_AMBER);
    drawTicks(160, 135, 100, 135, 405, 9, BMW_AMBER, 10);
    drawTicks(160, 135, 100, 340, 405, 3, COLOR_ALERT, 10);
    String r_str = String(rpm);
    tft.setFont(&Digital7_40pt7b);
    tft.setTextSize(1);
    tft.setTextColor(BMW_AMBER, TFT_BLACK);
    int rw = tft.textWidth(r_str);
    tft.setCursor(160 - (rw / 2), 155);
    tft.print(r_str);
    tft.setFont(nullptr);
  } else if (display_mode == 5) {
    if (redraw_static) {
      tft.fillScreen(TFT_BLACK);
      tft.fillRect(0, 0, 320, 45, COLOR_DARK_GREY);
      tft.setTextSize(3);
      tft.setTextColor(BMW_AMBER);
      tft.setCursor(20, 12);
      tft.print("SERVICE INTERVAL");
    }
    int total_hours = engine_total_seconds / 3600;
    int squares_gone = map(total_hours, 0, SERVICE_LIMIT_HOURS, 0, 8);
    if (squares_gone > 8) squares_gone = 8;
    for (int i = 0; i < 8; i++) {
      uint16_t col = COLOR_DARK_GREY;
      if (i >= squares_gone) {
        if (i < 5) col = COLOR_OK;
        else if (i == 5) col = BMW_AMBER;
        else col = COLOR_ALERT;
      }
      tft.fillRoundRect(25 + i * 35, 80, 30, 30, 4, col);
      tft.drawRoundRect(25 + i * 35, 80, 30, 30, 4, COLOR_FRAME);
    }
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(20, 140);
    tft.print("WORKED:");
    tft.setCursor(180, 140);
    tft.setTextColor(BMW_AMBER, TFT_BLACK);
    tft.print(total_hours);
    tft.print(" h");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(20, 170);
    tft.print("LIMIT:");
    tft.setCursor(180, 170);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.print(SERVICE_LIMIT_HOURS);
    tft.print(" h");
    String statusStr = "OK";
    uint16_t statusCol = COLOR_OK;
    if (squares_gone >= 5) {
      statusStr = "OIL SERVICE";
      statusCol = BMW_AMBER;
    }
    if (squares_gone >= 7) {
      statusStr = "INSPECTION!";
      statusCol = COLOR_ALERT;
    }
    tft.fillRect(0, 210, 320, 30, statusCol);
    tft.setTextSize(3);
    tft.setTextColor(TFT_BLACK);
    tft.setCursor(160 - (statusStr.length() * 9), 215);
    tft.print(statusStr);
  } else if (display_mode == 6) {
    if (redraw_static) {
      tft.fillScreen(TFT_BLACK);
      drawCenteredTitle("ACOUSTIC");
    }
    int mic = analogRead(PIN_MIC);
    int amp = abs(mic - 1950);
    for (int i = 0; i < 8; i++) {
      int mod = (i == 0 || i == 7) ? 60 : ((i == 1 || i == 6) ? 80 : 95);
      int h = map(constrain((amp * mod) / 100 + random(-5, 5), 300, 2500), 300, 2500, 0, 14);
      for (int b = 0; b < 14; b++) {
        int y = 200 - (b * 12);
        uint16_t col = COLOR_DARK_GREY;
        if (b < h) col = (b > 10) ? COLOR_ALERT : ((b > 7) ? BMW_AMBER : COLOR_OK);
        tft.fillRect(i * 40 + 2, y, 36, 10, col);
      }
    }
  } else if (display_mode == 7) {
    if (redraw_static) {
      tft.fillScreen(TFT_BLACK);
      tft.fillRect(0, 0, 320, 40, BMW_AMBER);
      tft.setTextSize(2);
      tft.setTextColor(TFT_BLACK);
      tft.setCursor(10, 12);
      tft.print(CITY_NAME);
      tft.setTextSize(3);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(10, 50);
      tft.print(weatherDesc);
    }
    drawWeatherIcon(260, 90, weatherCode);
    tft.setTextSize(6);
    tft.setTextColor(BMW_AMBER, TFT_BLACK);
    tft.setCursor(80, 170);
    tft.print(tempAir, 1);
    tft.setTextSize(2);
    tft.print(" C");
  } else if (display_mode == 8) {
    if (redraw_static) {
      tft.fillScreen(TFT_BLACK);
      drawCenteredTitle("SYSTEM STATUS");
      tft.drawLine(0, 40, 320, 40, BMW_AMBER);
      tft.setTextSize(2);
      tft.setTextColor(TFT_WHITE);
      tft.setCursor(10, 60);
      tft.print("IP: ");
      tft.println(WiFi.localIP());
      tft.setCursor(10, 100);
      tft.print("Volt Corr: ");
      tft.print(corr_volt);
      tft.print(" V");
      tft.setCursor(10, 130);
      tft.print("Temp Corr: ");
      tft.print(corr_temp);
      tft.print(" C");
    }
  }
  drawStatusBar(redraw_static);
}

void handleRoot() {
  String s = index_html;
  s.replace("%CT%", String(corr_temp));
  s.replace("%CR%", String(corr_rpm));
  s.replace("%CV%", String(corr_volt));
  s.replace("%BRI%", String(current_bri));
  s.replace("%CHK%", auto_bri ? "checked" : "");
  server.send(200, "text/html", s);
}
void handleSet() {
  if (server.hasArg("m")) {
    display_mode = server.arg("m").toInt();
    tft.fillScreen(TFT_BLACK);
    drawScreen(true);
    playTrack(1, 4);
  }
  server.send(200, "text/plain", "OK");
}
void handleSetCorr() {
  if (server.hasArg("t")) {
    corr_temp = server.arg("t").toInt();
    pref.putInt("ct", corr_temp);
  }
  if (server.hasArg("r")) {
    corr_rpm = server.arg("r").toInt();
    pref.putInt("cr", corr_rpm);
  }
  if (server.hasArg("v")) {
    corr_volt = server.arg("v").toFloat();
    pref.putFloat("cv", corr_volt);
  }
  playTrack(1, 4);
  server.sendHeader("Location", "/");
  server.send(303);
}
void handleSetBri() {
  if (server.hasArg("b")) {
    current_bri = server.arg("b").toInt();
    pref.putInt("bri", current_bri);
    tft.setBrightness(current_bri);
  }
  if (server.hasArg("a")) auto_bri = true;
  else auto_bri = false;
  pref.putBool("auto", auto_bri);
  server.sendHeader("Location", "/");
  server.send(303);
}
void handleReset() {
  engine_total_seconds = 0;
  pref.putUInt("sec", 0);
  server.send(200, "text/plain", "OK");
}
void handleReboot() {
  server.send(200, "text/plain", "OK");
  delay(100);
  ESP.restart();
}
void handleOTA() {
  server.send(200, "text/plain", "STARTING UPDATE...");
  performOTA();
}

void setup() {
  Serial.begin(115200);

  // 1. Инициализация железа
  tft.init(); 
  tft.setRotation(3);
  tft.setBrightness(70); // Пониженная яркость для старта
  
  myMP3.begin(9600, SERIAL_8N1, MP3_RX, MP3_TX); 
  delay(1000); 
  setVolume(27); 
  playTrack(1, 1);

  Wire.begin(PIN_SDA, PIN_SCL); 
  rtc.begin(); 
  sensors.begin(); 
  sensors.setWaitForConversion(false);
  
  pref.begin("bmw", false); 
  pulses_per_km = pref.getInt("sp", 4712); 
  engine_total_seconds = pref.getUInt("sec", 0);
  corr_temp = pref.getInt("ct", 0); 
  corr_rpm = pref.getInt("cr", 0); 
  corr_volt = pref.getFloat("cv", 0.0);
  current_bri = pref.getInt("bri", 255); 
  auto_bri = pref.getBool("auto", false);

  pinMode(PIN_BUTTON, INPUT_PULLUP); 
  pinMode(PIN_MIC, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_RPM), rpm_interrupt, FALLING); 
  attachInterrupt(digitalPinToInterrupt(PIN_SPEED), speed_interrupt, FALLING);
  
  // 2. Анимация (теперь Watchdog не мешает)
  showStartupAnimation();
  
  // 3. Сеть
  WiFi.begin(wifi_ssid, wifi_pass);
  if (WiFi.status() == WL_CONNECTED) updateWeather();

  server.on("/", handleRoot); 
  server.on("/set", handleSet); 
  server.on("/set_corr", handleSetCorr);
  server.on("/set_bri", handleSetBri); 
  server.on("/reset", handleReset);
  server.on("/reboot", handleReboot); 
  server.on("/ota", handleOTA); 
  server.begin();

  tft.setBrightness(current_bri); // Включаем яркость пользователя
  drawScreen(true);

  // 4. ВОТ ТЕПЕРЬ ВКЛЮЧАЕМ СТОРОЖЕВОГО ПСА (В САМОМ КОНЦЕ)
  // === FIX WATCHDOG ДЛЯ ESP32 v3.0+ ===
  esp_task_wdt_config_t twdt_config = {
      .timeout_ms = WDT_TIMEOUT * 1000,
      .idle_core_mask = (1 << 0), // Следим за ядром 0
      .trigger_panic = true
  };
  esp_task_wdt_init(&twdt_config);
  esp_task_wdt_add(NULL); 
  // ====================================
}

void loop() {
  esp_task_wdt_reset();  // ПИНАЕМ СТОРОЖЕВОГО ПСА (СБРОС ТАЙМЕРА)

  server.handleClient();
  static int last_reading = HIGH;
  static unsigned long last_db_time = 0;
  static int btn_state = HIGH;
  static unsigned long btn_press_start = 0;
  static bool long_press_handled = false;
  int reading = digitalRead(PIN_BUTTON);
  if (reading != last_reading) { last_db_time = millis(); }
  last_reading = reading;
  if ((millis() - last_db_time) > 50) {
    if (reading != btn_state) {
      btn_state = reading;
      if (btn_state == LOW) {
        btn_press_start = millis();
        long_press_handled = false;
      }
      if (btn_state == HIGH) {
        if (!long_press_handled) {
          display_mode++;
          if (display_mode > MAX_MODES) display_mode = 0;
          playTrack(1, 2);
          drawScreen(true);
        }
      }
    }
  }
  if (btn_state == LOW && !long_press_handled) {
    if (display_mode == 5 && (millis() - btn_press_start > 10000)) {
      engine_total_seconds = 0;
      pref.putUInt("sec", 0);
      playTrack(1, 5);
      tft.fillScreen(COLOR_OK);
      tft.setTextColor(TFT_BLACK);
      tft.setTextSize(4);
      tft.setCursor(60, 100);
      tft.print("RESET OK!");
      delay(1500);
      tft.fillScreen(TFT_BLACK);
      drawScreen(true);
      long_press_handled = true;
    }
  }

  if (millis() - timer_update > 200) {
    sensors.requestTemperatures();
    tempC = sensors.getTempCByIndex(0) + corr_temp;
    float adc_sum = 0;
    for (int i = 0; i < 20; i++) adc_sum += analogRead(PIN_VOLT);
    static float smooth_volt = 0;
    float inst_volt = ((adc_sum / 20.0) / 4095.0) * 3.3 * 5.6;
    if (smooth_volt == 0) smooth_volt = inst_volt;
    smooth_volt = (smooth_volt * 0.9) + (inst_volt * 0.1);
    volt = smooth_volt + corr_volt;
    noInterrupts();
    int p_rpm = rpm_pulses;
    rpm_pulses = 0;
    int p_spd = speed_pulses;
    speed_pulses = 0;
    interrupts();
    unsigned long dt = millis() - timer_update;
    timer_update = millis();
    rpm = ((p_rpm * 60 * 1000) / dt / 2) + corr_rpm;
    hist_temp[buf_idx] = (int)tempC;
    hist_volt[buf_idx] = (int)volt;
    hist_rpm[buf_idx] = rpm;
    buf_idx = (buf_idx + 1) % BUF_W;

    static unsigned long error_start_time = 0;
    static unsigned long last_strobe = 0;
    static bool alarm_blink = false;
    static unsigned long alarm_snd = 0;
    bool has_error = (tempC > 105 || (volt < 11.6 && volt > 5.0));
    if (has_error) {
      if (error_start_time == 0) error_start_time = millis();
      if (millis() - error_start_time > 2000) {
        if (millis() - last_strobe > 150) {
          last_strobe = millis();
          alarm_blink = !alarm_blink;
          if (alarm_blink) {
            tft.fillScreen(COLOR_ALERT);
            tft.setTextColor(TFT_WHITE, COLOR_ALERT);
            tft.setTextSize(5);
            tft.setCursor(40, 100);
            if (tempC > 105) tft.print("TEMP!!!");
            else tft.print("VOLT!!!");
          } else {
            tft.fillScreen(TFT_BLACK);
          }
        }
        if (alarm_snd == 0 || millis() - alarm_snd > 15000) {
          playTrack(1, 3);
          alarm_snd = millis();
          if (alarm_snd == 0) alarm_snd = 1;
        }
        return;
      }
    } else {
      error_start_time = 0;
      if (alarm_blink) {
        alarm_blink = false;
        tft.fillScreen(TFT_BLACK);
        drawScreen(true);
      }
    }
    if (display_mode != 1 && display_mode != 6 && display_mode != 7 && display_mode != 8) drawScreen(false);
  }
  if (display_mode == 1 && millis() % 1000 < 50) {
    drawScreen(false);
    delay(50);
  }
  if (display_mode == 6) {
    drawScreen(false);
    delay(30);
  }
  static unsigned long last_mh = 0;
  if (millis() - last_mh > 1000) {
    last_mh = millis();
    if (rpm > 300) {
      engine_total_seconds++;
      if (engine_total_seconds % 60 == 0) pref.putUInt("sec", engine_total_seconds);
    }
  }
}