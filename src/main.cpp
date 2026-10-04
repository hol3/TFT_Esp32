/*
 * Estacion meteorologica para ESP32 y pantalla redonda GC9A01.
 *
 * Flujo principal:
 * 1. El ESP32 crea el punto de acceso Clima-ESP32 y un servidor web.
 * 2. El usuario configura Wi-Fi y localidad desde 192.168.4.1.
 * 3. Open-Meteo convierte la localidad en coordenadas y entrega el clima.
 * 4. TFT_eSPI muestra textos e iconos PNG embebidos en el firmware.
 * 5. loop() anima el icono, actualiza el clima y recupera el Wi-Fi.
 */

// Bibliotecas del framework y de los servicios usados por el proyecto.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

// Valores de respaldo. Normalmente estos pines llegan desde platformio.ini.
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

// Datos del punto de acceso y periodos de las tareas expresados en milisegundos.
constexpr char AP_SSID[] = "Clima-ESP32";
constexpr char AP_PASSWORD[] = "clima1234";
constexpr unsigned long WEATHER_INTERVAL_MS = 15UL * 60UL * 1000UL;
constexpr unsigned long WIFI_RETRY_MS = 30UL * 1000UL;
constexpr unsigned long ANIMATION_INTERVAL_MS = 240;
constexpr uint16_t SCREEN_BACKGROUND = 0x0841;

// Objetos compartidos para pantalla, PNG, memoria no volatil y servidor HTTP.
TFT_eSPI display;
PNG pngDecoder;
Preferences preferences;
WebServer server(80);

// PlatformIO crea dos simbolos por PNG: inicio y final de sus datos en flash.
#define EMBEDDED_PNG(name, symbol)                                                                     \
  extern const uint8_t name##Start[] asm("_binary_src_images_icons_" symbol "_start");                 \
  extern const uint8_t name##End[] asm("_binary_src_images_icons_" symbol "_end")

EMBEDDED_PNG(iconCloud, "icons8_cloud_48_png");
EMBEDDED_PNG(iconNight, "icons8_night_48_png");
EMBEDDED_PNG(iconPartlyCloudy, "icons8_partly_cloudy_day_48_png");
EMBEDDED_PNG(iconSnow, "icons8_snow_48_png");
EMBEDDED_PNG(iconSnowStorm, "icons8_snow_storm_48_png");
EMBEDDED_PNG(iconSnowySunny, "icons8_snowy_sunny_day_48_png");
EMBEDDED_PNG(iconStorm, "icons8_storm_48_png");
EMBEDDED_PNG(iconSun, "icons8_sun_48_png");
EMBEDDED_PNG(iconUmbrella, "icons8_umbrella_48_png");
EMBEDDED_PNG(iconWindy, "icons8_windy_weather_48_png");

// Configuracion persistente introducida por el usuario desde la pagina web.
struct Settings {
  String ssid;                 // Nombre de la red a la que se conecta el ESP32.
  String password;             // Contrasena de esa red.
  String location;             // Texto buscado en el servicio de geocodificacion.
  double latitude = 0;         // Coordenada resuelta por Open-Meteo.
  double longitude = 0;        // Coordenada resuelta por Open-Meteo.
  bool hasCoordinates = false; // Evita geocodificar en cada actualizacion.
} settings;

// Ultima observacion meteorologica valida recibida desde Open-Meteo.
struct Weather {
  float temperature = 0;      // Temperatura actual en grados Celsius.
  float apparent = 0;         // Sensacion termica, disponible para futuras vistas.
  float humidity = 0;         // Humedad relativa en porcentaje.
  float wind = 0;             // Velocidad del viento en km/h.
  float minTemperature = 0;   // Minima prevista para hoy.
  float maxTemperature = 0;   // Maxima prevista para hoy.
  int code = -1;              // Codigo WMO que identifica el estado del cielo.
  int utcOffset = 0;          // Desfase horario de la localidad en segundos.
  bool isDay = true;          // Permite alternar entre los iconos de sol y luna.
  bool valid = false;         // Indica si ya existe una respuesta util.
  String error;               // Ultimo error visible en pantalla y pagina web.
} weather;

// Marcas de tiempo para ejecutar tareas sin bloquear el bucle principal.
unsigned long lastWeatherUpdate = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastAnimationUpdate = 0;

// Estado de la animacion y de la vista actualmente mostrada.
uint16_t animationFrame = 0;
uint16_t currentBackground = SCREEN_BACKGROUND;
bool showingWeather = false;
int16_t iconX = 96;
int16_t iconY = 51;
uint16_t pngLineBuffer[48]; // Una fila RGB565 del icono de 48 pixeles.

// Rango de memoria ocupado por un archivo PNG embebido.
struct EmbeddedImage {
  const uint8_t *start;
  const uint8_t *end;
};

// Pagina de configuracion guardada en flash para no consumir RAM permanente.
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

// Escapa caracteres con significado HTML antes de insertar valores del usuario.
String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("\"", "&quot;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  return value;
}

// Convierte texto UTF-8 a formato porcentual para usarlo en una URL.
String urlEncode(const String &value) {
  const char hex[] = "0123456789ABCDEF";
  String result;
  result.reserve(value.length() * 3); // El peor caso usa tres caracteres: %XX.
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    // RFC 3986 permite estos caracteres sin codificarlos.
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

/*
 * Dibuja texto centrado y lo recorta con puntos suspensivos si invade el borde.
 * El ancho disponible es la cuerda de un circulo de radio 116 en la altura Y.
 */
void centeredText(const String &text, int16_t y, uint8_t size, uint16_t color) {
  // La interfaz conserva una API sencilla de tres niveles tipograficos.
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
  // Teorema de Pitagoras: ancho = 2 * sqrt(radio^2 - distancia^2).
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

// Traduce los codigos meteorologicos WMO a descripciones breves en espanol.
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

// PNGdec usa el orden 00BBGGRR para mezclar correctamente el canal alfa.
uint32_t rgb565ToRgb888(uint16_t color) {
  const uint8_t red = ((color >> 11) & 0x1F) * 255 / 31;
  const uint8_t green = ((color >> 5) & 0x3F) * 255 / 63;
  const uint8_t blue = (color & 0x1F) * 255 / 31;
  return (static_cast<uint32_t>(blue) << 16) | (static_cast<uint32_t>(green) << 8) | red;
}

// Callback invocado por PNGdec una vez por cada fila decodificada del icono.
int drawPngLine(PNGDRAW *line) {
  // Convierte la fila a RGB565 y sustituye la transparencia por el fondo actual.
  pngDecoder.getLineAsRGB565(line, pngLineBuffer, PNG_RGB565_BIG_ENDIAN,
                             rgb565ToRgb888(currentBackground));
  // Enviar una sola fila mantiene el consumo de RAM bajo.
  display.pushImage(iconX, iconY + line->y, line->iWidth, 1, pngLineBuffer);
  return 1;
}

// Asocia el codigo WMO, el momento del dia y el viento con un PNG embebido.
EmbeddedImage selectWeatherIcon(int code) {
  // Los fenomenos mas severos se comprueban primero para darles prioridad.
  if (code >= 95) return {iconStormStart, iconStormEnd};
  if (code >= 85 && code <= 86) return {iconSnowStormStart, iconSnowStormEnd};
  if (code >= 71 && code <= 77) {
    return weather.isDay ? EmbeddedImage{iconSnowySunnyStart, iconSnowySunnyEnd}
                         : EmbeddedImage{iconSnowStart, iconSnowEnd};
  }
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    return {iconUmbrellaStart, iconUmbrellaEnd};
  }
  if (weather.wind >= 30 && code <= 3) return {iconWindyStart, iconWindyEnd};
  if (code == 0) {
    return weather.isDay ? EmbeddedImage{iconSunStart, iconSunEnd}
                         : EmbeddedImage{iconNightStart, iconNightEnd};
  }
  if (code == 1 || code == 2) return {iconPartlyCloudyStart, iconPartlyCloudyEnd};
  return {iconCloudStart, iconCloudEnd};
}

// Decodifica y dibuja el icono con un desplazamiento sinusoidal suave.
void drawWeatherIcon(int code, uint16_t frame) {
  const EmbeddedImage image = selectWeatherIcon(code);
  // Las dos frecuencias distintas producen un movimiento menos mecanico.
  iconX = 96 + round(sin(frame * 0.20) * 2.0);
  iconY = 51 + round(sin(frame * 0.14) * 2.0);
  const size_t imageSize = image.end - image.start;
  // openFLASH lee directamente el PNG incluido en firmware.bin.
  if (pngDecoder.openFLASH(const_cast<uint8_t *>(image.start), imageSize, drawPngLine) == PNG_SUCCESS) {
    pngDecoder.decode(nullptr, 0);
    pngDecoder.close();
  }
}

// Selecciona un fondo RGB565 relacionado con el estado meteorologico actual.
uint16_t weatherBackground(int code) {
  if (code == 0) return weather.isDay ? 0x1375 : 0x0864;     // Dia o noche
  if (code >= 95) return 0x20A7;                            // Tormenta purpura
  if (code == 45 || code == 48) return 0x5B2D;              // Niebla gris
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return 0x53F2;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return 0x1148;
  if (code >= 1 && code <= 3) return 0x42AD;                 // Nublado
  return SCREEN_BACKGROUND;
}

// Conserva solo el nombre principal para que la localidad quepa en el circulo.
String shortLocation() {
  String value = settings.location;
  const int comma = value.indexOf(',');
  if (comma > 0) value = value.substring(0, comma);
  if (value.length() > 18) value = value.substring(0, 17) + "...";
  return value;
}

// Compone la vista completa cada vez que llegan nuevos datos meteorologicos.
void drawWeather() {
  currentBackground = weatherBackground(weather.code);
  showingWeather = true;
  display.fillScreen(currentBackground);
  centeredText(shortLocation(), 19, 2, 0xBDF7);
  animationFrame = 0; // Toda condicion nueva comienza desde el primer fotograma.
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

// Muestra estados transitorios como conexion, actualizacion o errores.
void drawMessage(const String &title, const String &line1, const String &line2 = "") {
  showingWeather = false; // Pausa la animacion mientras esta vista es visible.
  display.fillScreen(SCREEN_BACKGROUND);
  centeredText(title, 61, 2, 0xFEC0);
  centeredText(line1, 105, 1, 0xFFFF);
  if (line2.length()) centeredText(line2, 123, 1, 0xFFFF);
  centeredText("AP: Clima-ESP32", 165, 1, 0xBDF7);
  centeredText("192.168.4.1", 183, 1, 0xBDF7);
}

/*
 * Ejecuta una peticion HTTPS y deserializa su respuesta JSON.
 * Devuelve false y rellena `error` ante cualquier fallo de red o contenido.
 */
bool getJson(const String &url, JsonDocument &document, String &error) {
  WiFiClientSecure client;
  // Los datos son publicos; se omite validar el certificado para ahorrar flash.
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(12000); // Evita bloquear indefinidamente si la API no responde.
  if (!http.begin(client, url)) {
    error = "No se pudo iniciar HTTPS";
    return false;
  }
  http.addHeader("Accept", "application/json");
  // El ESP32 puede leer esta respuesta sin implementar descompresion gzip.
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

  // ArduinoJson crea un arbol consultable directamente desde la respuesta.
  const DeserializationError jsonError = deserializeJson(document, payload);
  if (jsonError) {
    error = "JSON: " + String(jsonError.c_str());
    Serial.printf("GET %s: %s (%u bytes)\n", url.c_str(), error.c_str(), payload.length());
    Serial.println(payload.substring(0, 200));
    return false;
  }
  return true;
}

// Resuelve el texto de la localidad a latitud y longitud mediante Open-Meteo.
bool findCoordinates() {
  JsonDocument document;
  String error;
  const String url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
                     urlEncode(settings.location) + "&count=1&language=es&format=json";
  if (!getJson(url, document, error)) {
    weather.error = error;
    return false;
  }
  JsonObject result = document["results"][0]; // Se usa la coincidencia principal.
  if (result.isNull()) {
    weather.error = "Localidad no encontrada";
    return false;
  }
  settings.latitude = result["latitude"].as<double>();
  settings.longitude = result["longitude"].as<double>();
  settings.hasCoordinates = true;
  // Guardar las coordenadas reduce peticiones y acelera los siguientes arranques.
  preferences.putDouble("lat", settings.latitude);
  preferences.putDouble("lon", settings.longitude);
  preferences.putBool("hasCoord", true);
  return true;
}

// Descarga la observacion actual y el minimo/maximo diario de Open-Meteo.
bool updateWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    weather.error = "Sin conexion Wi-Fi";
    return false;
  }
  // Solo geocodifica si la localidad es nueva o aun no tiene coordenadas.
  if (!settings.hasCoordinates && !findCoordinates()) return false;

  JsonDocument document;
  String error;
  const String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(settings.latitude, 6) +
                     "&longitude=" + String(settings.longitude, 6) +
                     "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,is_day"
                     "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=3";
  if (!getJson(url, document, error)) {
    weather.error = error;
    return false;
  }

  // El operador `|` de ArduinoJson proporciona un valor seguro si falta un campo.
  weather.temperature = document["current"]["temperature_2m"] | 0.0;
  weather.apparent = document["current"]["apparent_temperature"] | 0.0;
  weather.humidity = document["current"]["relative_humidity_2m"] | 0.0;
  weather.wind = document["current"]["wind_speed_10m"] | 0.0;
  weather.code = document["current"]["weather_code"] | -1;
  weather.isDay = (document["current"]["is_day"] | 1) == 1;
  weather.maxTemperature = document["daily"]["temperature_2m_max"][0] | 0.0;
  weather.minTemperature = document["daily"]["temperature_2m_min"][0] | 0.0;
  weather.utcOffset = document["utc_offset_seconds"] | 0;
  weather.valid = true;
  weather.error = "";
  lastWeatherUpdate = millis();
  // Sincroniza el reloj con el huso horario que corresponde a la localidad.
  configTime(weather.utcOffset, 0, "pool.ntp.org", "time.nist.gov");
  drawWeather();
  return true;
}

// Recupera desde NVS la configuracion que sobrevive a reinicios y cortes de luz.
void loadSettings() {
  preferences.begin("weather", false); // `false` abre el espacio en lectura/escritura.
  settings.ssid = preferences.getString("ssid", "");
  settings.password = preferences.getString("password", "");
  settings.location = preferences.getString("location", "Madrid, Espana");
  settings.latitude = preferences.getDouble("lat", 0);
  settings.longitude = preferences.getDouble("lon", 0);
  settings.hasCoordinates = preferences.getBool("hasCoord", false);
}

// Inicia Wi-Fi y espera como maximo `timeoutMs`, atendiendo la web entretanto.
bool connectWifi(unsigned long timeoutMs = 15000) {
  if (!settings.ssid.length()) return false;
  WiFi.begin(settings.ssid.c_str(), settings.password.c_str());
  lastWifiAttempt = millis();
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) {
    // La pagina de configuracion sigue disponible durante el intento de conexion.
    server.handleClient();
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Construye la pagina principal sustituyendo sus marcadores por valores actuales.
void handleRoot() {
  String page = FPSTR(PAGE_HTML); // Copia temporal desde flash para poder modificarla.
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

// Valida y guarda el formulario enviado mediante POST a /save.
void handleSave() {
  // Rechaza peticiones incompletas antes de modificar la configuracion existente.
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

  // Estas banderas determinan que recursos deben reconectarse o recalcularse.
  const bool networkChanged = newSsid != settings.ssid || server.arg("password").length();
  const bool locationChanged = !newLocation.equalsIgnoreCase(settings.location);
  settings.ssid = newSsid;
  settings.location = newLocation;
  // Una contrasena vacia conserva la anterior y evita mostrarla en el formulario.
  if (server.arg("password").length()) settings.password = server.arg("password");
  if (locationChanged) settings.hasCoordinates = false;

  // Preferences escribe estos valores en la memoria no volatil del ESP32.
  preferences.putString("ssid", settings.ssid);
  preferences.putString("password", settings.password);
  preferences.putString("location", settings.location);
  preferences.putBool("hasCoord", settings.hasCoordinates);

  // Patron Post/Redirect/Get: evita reenviar el formulario al recargar la pagina.
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

// Registra las rutas HTTP y redirige cualquier ruta desconocida al formulario.
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}

// Arduino ejecuta setup una sola vez despues de encender o reiniciar el ESP32.
void setup() {
  Serial.begin(115200); // Canal de diagnostico visible en el monitor serie.
  // La retroiluminacion se controla como una salida digital siempre encendida.
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  display.init(); // TFT_eSPI configura SPI y el controlador GC9A01.
  display.setRotation(0);
  display.setTextWrap(false, false);
  drawMessage("Iniciando", "Clima ESP32");

  loadSettings();
  // AP_STA mantiene simultaneamente el portal local y la conexion a Internet.
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

// Arduino repite loop continuamente despues de completar setup.
void loop() {
  // Debe llamarse con frecuencia para responder a navegadores conectados.
  server.handleClient();

  // millis() permite animar sin delay y sin bloquear el servidor web.
  if (weather.valid && showingWeather && millis() - lastAnimationUpdate >= ANIMATION_INTERVAL_MS) {
    lastAnimationUpdate = millis();
    ++animationFrame;
    // Solo limpia la region del icono para no redibujar ni parpadear toda la vista.
    display.fillRect(68, 34, 104, 84, currentBackground);
    drawWeatherIcon(weather.code, animationFrame);
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Actualiza inmediatamente al arrancar y despues cada quince minutos.
    if (!weather.valid || millis() - lastWeatherUpdate >= WEATHER_INTERVAL_MS) {
      if (!updateWeather() && !weather.valid) drawMessage("Sin datos", weather.error);
    }
  } else if (settings.ssid.length() && millis() - lastWifiAttempt >= WIFI_RETRY_MS) {
    // Si se pierde la red, reintenta en segundo plano cada treinta segundos.
    WiFi.disconnect();
    WiFi.begin(settings.ssid.c_str(), settings.password.c_str());
    lastWifiAttempt = millis();
  }
  delay(2); // Cede tiempo al sistema Wi-Fi y al planificador del ESP32.
}
