# Estación meteorológica ESP32 + GC9A01

Proyecto para mostrar el tiempo actual en una pantalla TFT redonda GC9A01 de 1,28 pulgadas (240 x 240). Incluye una interfaz web para configurar la red Wi-Fi y la localidad, sin necesidad de una clave de API meteorológica.

## Material

- ESP32 DevKit V1
- Pantalla TFT redonda GC9A01 de 1,28 pulgadas
- Cables Dupont
- Fuente USB estable

## Cableado

| GC9A01 | ESP32 | Función |
|---|---:|---|
| VCC | 3V3 | Alimentación |
| GND | GND | Tierra |
| SCL / CLK | GPIO 18 | Reloj SPI |
| SDA / DIN | GPIO 23 | Datos SPI (MOSI) |
| CS | GPIO 5 | Chip select |
| DC | GPIO 16 | Datos / comando |
| RST | GPIO 4 | Reset |
| BL / BLK | GPIO 17 | Retroiluminación |

La pantalla debe funcionar con lógica de 3,3 V. Si tu módulo tiene otros pines disponibles, modifica las macros `TFT_*` de `platformio.ini`.

## Carga

1. Instala [Visual Studio Code](https://code.visualstudio.com/) y la extensión PlatformIO.
2. Abre esta carpeta como proyecto.
3. Conecta el ESP32 por USB.
4. Ejecuta `PlatformIO: Upload` o, desde una terminal con PlatformIO instalado, `pio run -t upload`.

El monitor serie funciona a 115200 baudios.

## Primera configuración

1. Enciende el ESP32.
2. Conecta el teléfono u ordenador a la red Wi-Fi `Clima-ESP32`.
3. Usa la contraseña `clima1234`.
4. Abre `http://192.168.4.1` en el navegador.
5. Introduce el nombre de la red Wi-Fi, su contraseña y una localidad como `Valencia, España`.
6. Pulsa **Guardar y actualizar**.

El punto de acceso de configuración permanece activo para poder cambiar la localidad posteriormente. Las opciones se guardan en la memoria no volátil del ESP32.

## Datos meteorológicos

El firmware usa los servicios gratuitos de geocodificación y predicción de [Open-Meteo](https://open-meteo.com/). Actualiza la información cada 15 minutos y muestra:

- Estado del cielo e icono
- Temperatura actual
- Temperatura mínima y máxima del día
- Humedad relativa
- Velocidad del viento

La conexión HTTPS se establece sin validar el certificado del servidor para evitar almacenar y mantener certificados raíz en el microcontrolador. Úsalo únicamente para estos datos públicos, no para transmitir información sensible.
