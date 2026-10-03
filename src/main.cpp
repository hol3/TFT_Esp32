#include <Arduino.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#ifndef TFT_MOSI
#define TFT_MOSI 23
#endif
#ifndef TFT_SCLK
#define TFT_SCLK 18
#endif
#ifndef TFT_CS
#define TFT_CS 5
#endif
#ifndef TFT_DC
#define TFT_DC 16
#endif
#ifndef TFT_RST
#define TFT_RST 4
#endif
#ifndef TFT_BL
#define TFT_BL 17
#endif

constexpr char AP_SSID[] = "Clima-ESP32";
constexpr char AP_PASSWORD[] = "clima1234";
constexpr unsigned long WEATHER_INTERVAL_MS = 15UL * 60UL * 1000UL;
constexpr unsigned long WIFI_RETRY_MS = 30UL * 1000UL;
constexpr unsigned long ANIMATION_INTERVAL_MS = 120;
constexpr uint16_t SCREEN_BACKGROUND = 0x0841;

TFT_eSPI display;
Preferences preferences;
WebServer server(80);

struct Settings {
  String ssid;
  String password;
  String location;
  double latitude = 0;
  double longitude = 0;
  bool hasCoordinates = false;
} settings;

struct Weather {
  float temperature = 0;
  float apparent = 0;
  float humidity = 0;
  float wind = 0;
  float minTemperature = 0;
  float maxTemperature = 0;
  int code = -1;
  int utcOffset = 0;
  bool valid = false;
  String error;
} weather;

unsigned long lastWeatherUpdate = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastAnimationUpdate = 0;
uint16_t animationFrame = 0;
uint16_t currentBackground = SCREEN_BACKGROUND;
bool showingWeather = false;

const char PAGE_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Clima ESP32</title><style>
:root{color-scheme:dark;--ink:#f6f0df;--muted:#a8b8b0;--accent:#ffb454;--panel:#132c2c}
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:22px;
font-family:system-ui,sans-serif;color:var(--ink);background:radial-gradient(circle at 20% 10%,#245653,#091716 55%)}
main{width:min(100%,480px);padding:30px;border:1px solid #37635c;border-radius:28px;background:#0d2220e8;box-shadow:0 24px 70px #0008}
.eyebrow{margin:0;color:var(--accent);font-size:.75rem;font-weight:800;letter-spacing:.18em;text-transform:uppercase}
h1{margin:.3rem 0 .4rem;font-size:2.2rem;line-height:1}p{color:var(--muted);line-height:1.5}
.status{display:flex;gap:10px;margin:22px 0;padding:13px 15px;border-radius:15px;background:var(--panel);font-size:.9rem}
.dot{width:10px;height:10px;margin-top:5px;border-radius:50%;background:STATUS_COLOR;box-shadow:0 0 12px STATUS_COLOR}
label{display:block;margin:17px 0 7px;font-size:.8rem;font-weight:800;letter-spacing:.06em;text-transform:uppercase}
input{width:100%;padding:14px;border:1px solid #41645f;border-radius:12px;color:var(--ink);background:#091817;font:inherit;outline:none}
input:focus{border-color:var(--accent)}button{width:100%;margin-top:24px;padding:15px;border:0;border-radius:13px;background:var(--accent);color:#18201d;font:800 1rem system-ui;cursor:pointer}
small{display:block;margin-top:17px;color:#80938e;text-align:center}.place{color:var(--ink);font-weight:700}
</style></head><body><main><p class="eyebrow">Pantalla meteorológica</p><h1>Clima ESP32</h1>
<p>Configura la red y la localidad que aparecerá en tu pantalla redonda.</p>
<div class="status"><span class="dot"></span><span>STATUS_TEXT</span></div>
<form method="post" action="/save"><label for="ssid">Red Wi-Fi</label>
<input id="ssid" name="ssid" maxlength="32" required value="SSID_VALUE" placeholder="Nombre de la red">
<label for="password">Contraseña</label><input id="password" type="password" name="password" maxlength="64" placeholder="Déjala vacía para conservarla">
<label for="location">Localidad</label><input id="location" name="location" maxlength="80" required value="LOCATION_VALUE" placeholder="Ej. Sevilla, España">
<button type="submit">Guardar y actualizar</button></form>
<small>Conectado al punto de acceso <span class="place">Clima-ESP32</span> · 192.168.4.1</small></main></body></html>
)HTML";

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("\"", "&quot;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  return value;
}

String urlEncode(const String &value) {
  const char hex[] = "0123456789ABCDEF";
  String result;
  result.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      result += static_cast<char>(c);
    } else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 0x0F];
    }
  }
  return result;
}

void centeredText(const String &text, int16_t y, uint8_t size, uint16_t color) {
  const GFXfont *font = &FreeSans9pt7b;
  if (size == 2) font = &FreeSansBold9pt7b;
  if (size >= 4) font = &FreeSansBold18pt7b;
  display.setFreeFont(font);
  display.setTextColor(color);

  String rendered = text;
  uint16_t width = display.textWidth(rendered);
  const uint16_t height = display.fontHeight();
  const int16_t centerY = y + height / 2;
  const int16_t distanceToCenter = abs(centerY - 120);
  const uint16_t maxWidth = distanceToCenter < 116
                                ? 2 * sqrt(116L * 116L - distanceToCenter * distanceToCenter) - 10
                                : 0;
  while (width > maxWidth && rendered.length() > 4) {
    rendered.remove(rendered.length() - 4);
    rendered += "...";
    width = display.textWidth(rendered);
  }
  display.setTextDatum(TC_DATUM);
  display.drawString(rendered, 120, y);
}

String weatherDescription(int code) {
  if (code == 0) return "Despejado";
  if (code <= 3) return "Parcial nublado";
  if (code == 45 || code == 48) return "Niebla";
  if (code >= 51 && code <= 57) return "Llovizna";
  if (code >= 61 && code <= 67) return "Lluvia";
  if (code >= 71 && code <= 77) return "Nieve";
  if (code >= 80 && code <= 82) return "Chubascos";
  if (code >= 85 && code <= 86) return "Nieve";
  if (code >= 95) return "Tormenta";
  return "Sin datos";
}

void drawCloud(int x, int y, uint16_t color) {
  display.fillCircle(x, y + 7, 14, color);
  display.fillCircle(x + 17, y, 18, color);
  display.fillCircle(x + 36, y + 8, 13, color);
  display.fillRoundRect(x - 1, y + 7, 49, 20, 9, color);
}

void drawWeatherIcon(int code, uint16_t frame) {
  constexpr uint16_t yellow = 0xFEC0;
  constexpr uint16_t cloud = 0xD69A;
  constexpr uint16_t rain = 0x3DDF;
  const int cx = 120;
  const int cy = 75;
  const int cloudOffset = round(sin(frame * 0.18) * 2.0);

  if (code == 0) {
    display.fillCircle(cx, cy, 22 + ((frame / 5) % 2), yellow);
    for (int angle = 0; angle < 360; angle += 45) {
      const float radians = (angle + frame * 4) * PI / 180.0;
      display.drawLine(cx + cos(radians) * 31, cy + sin(radians) * 31,
                       cx + cos(radians) * 40, cy + sin(radians) * 40, yellow);
    }
    return;
  }

  if (code >= 95) {
    drawCloud(94 + cloudOffset, 55, cloud);
    const uint16_t boltColor = frame % 12 < 3 ? 0xFFFF : yellow;
    display.fillTriangle(122, 83, 109, 108, 122, 105, boltColor);
    display.fillTriangle(122, 103, 113, 114, 135, 96, boltColor);
    return;
  }

  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    drawCloud(94 + cloudOffset, 54, cloud);
    for (int index = 0; index < 3; ++index) {
      const int x = 101 + index * 18;
      const int y = 89 + ((frame * 3 + index * 7) % 17);
      display.drawLine(x, y, x - 4, y + 9, rain);
    }
    return;
  }

  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
    drawCloud(94 + cloudOffset, 52, cloud);
    for (int index = 0; index < 3; ++index) {
      const int x = 103 + index * 17 + ((frame + index) % 3) - 1;
      const int y = 91 + ((frame * 2 + index * 7) % 18);
      display.drawLine(x - 3, y, x + 3, y, 0xFFFF);
      display.drawLine(x, y - 3, x, y + 3, 0xFFFF);
    }
    return;
  }

  if (code == 45 || code == 48) {
    drawCloud(94 + cloudOffset, 48, cloud);
    const int fogOffset = frame % 12;
    display.drawFastHLine(82 + fogOffset, 88, 65, cloud);
    display.drawFastHLine(92 - fogOffset, 98, 65, cloud);
    display.drawFastHLine(82 + fogOffset, 108, 65, cloud);
    return;
  }

  if (code > 0 && code <= 2) {
    display.fillCircle(101, 62, 18, yellow);
    for (int angle = 0; angle < 360; angle += 90) {
      const float radians = (angle + frame * 5) * PI / 180.0;
      display.drawLine(101 + cos(radians) * 22, 62 + sin(radians) * 22,
                       101 + cos(radians) * 28, 62 + sin(radians) * 28, yellow);
    }
    drawCloud(101 + cloudOffset, 62, cloud);
  } else {
    drawCloud(95 + cloudOffset, 57, cloud);
  }
}

uint16_t weatherBackground(int code) {
  if (code == 0) return 0x1375;                              // Cielo azul
  if (code >= 95) return 0x20A7;                            // Tormenta purpura
  if (code == 45 || code == 48) return 0x5B2D;              // Niebla gris
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return 0x53F2;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return 0x1148;
  if (code >= 1 && code <= 3) return 0x42AD;                 // Nublado
  return SCREEN_BACKGROUND;
}

String shortLocation() {
  String value = settings.location;
  const int comma = value.indexOf(',');
  if (comma > 0) value = value.substring(0, comma);
  if (value.length() > 18) value = value.substring(0, 17) + "...";
  return value;
}

void drawWeather() {
  currentBackground = weatherBackground(weather.code);
  showingWeather = true;
  display.fillScreen(currentBackground);
  centeredText(shortLocation(), 19, 2, 0xBDF7);
  animationFrame = 0;
  drawWeatherIcon(weather.code, animationFrame);

  char temperature[12];
  snprintf(temperature, sizeof(temperature), "%.0f C", weather.temperature);
  centeredText(temperature, 119, 4, 0xFFFF);
  centeredText(weatherDescription(weather.code), 158, 2, 0xFEC0);

  char details[42];
  snprintf(details, sizeof(details), "Min %.0f  Max %.0f", weather.minTemperature, weather.maxTemperature);
  centeredText(details, 188, 1, 0xBDF7);
  snprintf(details, sizeof(details), "Hum %.0f%%  %.0f km/h", weather.humidity, weather.wind);
  centeredText(details, 207, 1, 0xBDF7);
}

void drawMessage(const String &title, const String &line1, const String &line2 = "") {
  showingWeather = false;
  display.fillScreen(SCREEN_BACKGROUND);
  centeredText(title, 61, 2, 0xFEC0);
  centeredText(line1, 105, 1, 0xFFFF);
  if (line2.length()) centeredText(line2, 123, 1, 0xFFFF);
  centeredText("AP: Clima-ESP32", 165, 1, 0xBDF7);
  centeredText("192.168.4.1", 183, 1, 0xBDF7);
}

bool getJson(const String &url, JsonDocument &document, String &error) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(client, url)) {
    error = "No se pudo iniciar HTTPS";
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    error = "Error HTTP " + String(status);
    Serial.printf("GET %s: %s\n", url.c_str(), error.c_str());
    http.end();
    return false;
  }

  // HTTPClient decodifica aqui las respuestas con transferencia fragmentada.
  const String payload = http.getString();
  http.end();
  if (!payload.length()) {
    error = "Respuesta vacia del servidor";
    Serial.printf("GET %s: respuesta vacia\n", url.c_str());
    return false;
  }

  const DeserializationError jsonError = deserializeJson(document, payload);
  if (jsonError) {
    error = "JSON: " + String(jsonError.c_str());
    Serial.printf("GET %s: %s (%u bytes)\n", url.c_str(), error.c_str(), payload.length());
    Serial.println(payload.substring(0, 200));
    return false;
  }
  return true;
}

bool findCoordinates() {
  JsonDocument document;
  String error;
  const String url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
                     urlEncode(settings.location) + "&count=1&language=es&format=json";
  if (!getJson(url, document, error)) {
    weather.error = error;
    return false;
  }
  JsonObject result = document["results"][0];
  if (result.isNull()) {
    weather.error = "Localidad no encontrada";
    return false;
  }
  settings.latitude = result["latitude"].as<double>();
  settings.longitude = result["longitude"].as<double>();
  settings.hasCoordinates = true;
  preferences.putDouble("lat", settings.latitude);
  preferences.putDouble("lon", settings.longitude);
  preferences.putBool("hasCoord", true);
  return true;
}

bool updateWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    weather.error = "Sin conexion Wi-Fi";
    return false;
  }
  if (!settings.hasCoordinates && !findCoordinates()) return false;

  JsonDocument document;
  String error;
  const String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(settings.latitude, 6) +
                     "&longitude=" + String(settings.longitude, 6) +
                     "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
                     "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=3";
  if (!getJson(url, document, error)) {
    weather.error = error;
    return false;
  }

  weather.temperature = document["current"]["temperature_2m"] | 0.0;
  weather.apparent = document["current"]["apparent_temperature"] | 0.0;
  weather.humidity = document["current"]["relative_humidity_2m"] | 0.0;
  weather.wind = document["current"]["wind_speed_10m"] | 0.0;
  weather.code = document["current"]["weather_code"] | -1;
  weather.maxTemperature = document["daily"]["temperature_2m_max"][0] | 0.0;
  weather.minTemperature = document["daily"]["temperature_2m_min"][0] | 0.0;
  weather.utcOffset = document["utc_offset_seconds"] | 0;
  weather.valid = true;
  weather.error = "";
  lastWeatherUpdate = millis();
  configTime(weather.utcOffset, 0, "pool.ntp.org", "time.nist.gov");
  drawWeather();
  return true;
}

void loadSettings() {
  preferences.begin("weather", false);
  settings.ssid = preferences.getString("ssid", "");
  settings.password = preferences.getString("password", "");
  settings.location = preferences.getString("location", "Madrid, Espana");
  settings.latitude = preferences.getDouble("lat", 0);
  settings.longitude = preferences.getDouble("lon", 0);
  settings.hasCoordinates = preferences.getBool("hasCoord", false);
}

bool connectWifi(unsigned long timeoutMs = 15000) {
  if (!settings.ssid.length()) return false;
  WiFi.begin(settings.ssid.c_str(), settings.password.c_str());
  lastWifiAttempt = millis();
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) {
    server.handleClient();
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

void handleRoot() {
  String page = FPSTR(PAGE_HTML);
  const bool connected = WiFi.status() == WL_CONNECTED;
  String status = connected ? "Conectado a <b>" + htmlEscape(WiFi.SSID()) + "</b> · IP " + WiFi.localIP().toString()
                            : "Sin conexión a Internet. Configura una red Wi-Fi.";
  if (weather.error.length()) status += "<br>" + htmlEscape(weather.error);
  page.replace("STATUS_TEXT", status);
  page.replace("STATUS_COLOR", connected ? "#6ee7a2" : "#ff826f");
  page.replace("SSID_VALUE", htmlEscape(settings.ssid));
  page.replace("LOCATION_VALUE", htmlEscape(settings.location));
  server.send(200, "text/html; charset=utf-8", page);
}

void handleSave() {
  if (!server.hasArg("ssid") || !server.hasArg("location")) {
    server.send(400, "text/plain; charset=utf-8", "Faltan datos obligatorios");
    return;
  }

  String newSsid = server.arg("ssid");
  String newLocation = server.arg("location");
  newSsid.trim();
  newLocation.trim();
  if (!newSsid.length() || !newLocation.length()) {
    server.send(400, "text/plain; charset=utf-8", "SSID y localidad son obligatorios");
    return;
  }

  const bool networkChanged = newSsid != settings.ssid || server.arg("password").length();
  const bool locationChanged = !newLocation.equalsIgnoreCase(settings.location);
  settings.ssid = newSsid;
  settings.location = newLocation;
  if (server.arg("password").length()) settings.password = server.arg("password");
  if (locationChanged) settings.hasCoordinates = false;

  preferences.putString("ssid", settings.ssid);
  preferences.putString("password", settings.password);
  preferences.putString("location", settings.location);
  preferences.putBool("hasCoord", settings.hasCoordinates);

  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
  delay(150);

  if (networkChanged || WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    drawMessage("Conectando", settings.ssid);
    connectWifi();
  }
  if (WiFi.status() == WL_CONNECTED) {
    drawMessage("Actualizando", shortLocation());
    if (!updateWeather()) drawMessage("Sin datos", weather.error);
  } else {
    drawMessage("Configura Wi-Fi", "Abre 192.168.4.1");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  display.init();
  display.setRotation(0);
  display.setTextWrap(false, false);
  drawMessage("Iniciando", "Clima ESP32");

  loadSettings();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  setupWebServer();

  if (connectWifi()) {
    drawMessage("Actualizando", shortLocation());
    if (!updateWeather()) drawMessage("Sin datos", weather.error);
  } else {
    drawMessage("Configura Wi-Fi", "Clave: clima1234", "Abre 192.168.4.1");
  }
}

void loop() {
  server.handleClient();

  if (weather.valid && showingWeather && millis() - lastAnimationUpdate >= ANIMATION_INTERVAL_MS) {
    lastAnimationUpdate = millis();
    ++animationFrame;
    display.fillRect(68, 34, 104, 84, currentBackground);
    drawWeatherIcon(weather.code, animationFrame);
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!weather.valid || millis() - lastWeatherUpdate >= WEATHER_INTERVAL_MS) {
      if (!updateWeather() && !weather.valid) drawMessage("Sin datos", weather.error);
    }
  } else if (settings.ssid.length() && millis() - lastWifiAttempt >= WIFI_RETRY_MS) {
    WiFi.disconnect();
    WiFi.begin(settings.ssid.c_str(), settings.password.c_str());
    lastWifiAttempt = millis();
  }
  delay(2);
}
