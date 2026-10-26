# MP3 casero

Reproductor MP3 hecho con una ESP32-WROOM que lee canciones de una microSD y las envía por Bluetooth a unos audífonos (por ejemplo, unos AirPods). Muestra la canción en una pantalla SH1106 de 1.3" y se controla con 3 botones.

El firmware está en [`MP3_Bluetooth/MP3_Bluetooth.ino`](MP3_Bluetooth/MP3_Bluetooth.ino) y se abre con Arduino IDE.

## Componentes

- ESP32-WROOM (DevKit)
- Pantalla 1.3" SH1106 128x64 (I2C)
- Lector microSD SPI y tarjeta microSD de 8 GB (FAT32)
- 3 botones
- Switch, batería LiPo 3.7 V 800 mAh, regulador TPS63070 y cargador TP4056 con protección

## Conexiones

| Componente | Pin del módulo | Pin de la ESP32 |
|---|---|---|
| Pantalla SH1106 | VCC | 3V3 |
| | GND | GND |
| | SCK | D22 |
| | SDA | D21 |
| Lector microSD | CS | D5 |
| | SCK | D18 |
| | MOSI | D23 |
| | MISO | D19 |
| | VCC | VIN (5 V) |
| | GND | GND |
| Botón 1: anterior | una pata | D13 (la otra a GND) |
| Botón 2: play/pausa | una pata | D14 (la otra a GND) |
| Botón 3: siguiente | una pata | D27 (la otra a GND) |

Los botones no necesitan resistencias: el código activa las resistencias pull-up internas de la ESP32.

## Controles

| Botón | Acción |
|---|---|
| Botón 1 | Canción anterior (desde la primera salta a la última) |
| Botón 2 | Pausa / play |
| Botón 3 | Canción siguiente (desde la última vuelve a la primera) |

Al terminar la última canción, la lista vuelve a empezar.

## Instalación en Arduino IDE

1. Instala el soporte para ESP32: en **Herramientas > Placa > Gestor de tarjetas**, busca `esp32` de Espressif e instálalo.
2. Instala las librerías:
   - **U8g2**: desde **Programa > Incluir librería > Administrar bibliotecas**.
   - **audio-tools**, **ESP32-A2DP** y **arduino-libhelix**: no están en el gestor. Descarga cada una como ZIP (botón verde **Code > Download ZIP** en GitHub) y agrégalas con **Programa > Incluir librería > Añadir biblioteca .ZIP**.
     - https://github.com/pschatzmann/arduino-audio-tools
     - https://github.com/pschatzmann/ESP32-A2DP
     - https://github.com/pschatzmann/arduino-libhelix
3. Abre `MP3_Bluetooth/MP3_Bluetooth.ino`.
4. En **Herramientas** elige:
   - Placa: **ESP32 Dev Module**
   - Partition Scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**. El Bluetooth y el decodificador MP3 no caben en la partición por defecto.
5. Sube el programa. El Monitor Serie (115200 baudios) muestra mensajes útiles si algo falla.

## Preparar la microSD

- Formato **FAT32**.
- Copia los archivos `.mp3` (pueden ir en carpetas). Funcionan mejor a **44.1 kHz estéreo**, que es lo que usa el Bluetooth; los MP3 a 48 kHz se escucharán un poco más lentos y graves.
- Si el MP3 tiene etiquetas ID3, la pantalla muestra el título y el artista; si no, muestra el nombre del archivo.
- La primera vez, la librería crea un pequeño archivo de índice en la tarjeta para encontrar las canciones más rápido.

## Conectar los audífonos

1. Pon los AirPods en modo de emparejamiento: con los AirPods dentro del estuche y la tapa abierta, mantén presionado el botón trasero hasta que la luz parpadee en blanco.
2. Enciende el MP3. La pantalla dice "Buscando AirPods" hasta que se conecta y luego "BT conectado".

El MP3 se conecta al primer dispositivo cuyo nombre **empieza** con `AirPods`, así que "AirPods de Thalia" o "AirPods Pro" también sirven. Para usar otros audífonos, cambia `BT_DEVICE_NAME` al inicio del sketch. El volumen inicial se cambia en `VOLUMEN_INICIAL`.

## Notas

- La ESP32 solo hace Bluetooth clásico (A2DP) con SBC; los AirPods lo soportan, pero no esperes la calidad de AAC.
- El lector microSD está alimentado desde VIN (5 V). Esto funciona si el módulo tiene regulador a 3.3 V. Si alimentas todo desde la batería a través del TPS63070, revisa que VIN realmente reciba voltaje suficiente para ese regulador.
