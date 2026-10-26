/*
 * MP3 casero - ESP32-WROOM
 *
 * Lee los .mp3 de la microSD, los decodifica y los envia por Bluetooth (A2DP)
 * a unos audifonos cuyo nombre empiece con BT_DEVICE_NAME. Muestra la cancion
 * en la pantalla SH1106 y se controla con 3 botones:
 *   Boton 1 (D13): cancion anterior
 *   Boton 2 (D14): pausa / play
 *   Boton 3 (D27): cancion siguiente
 *
 * Librerias (ver README.md):
 *   - audio-tools   https://github.com/pschatzmann/arduino-audio-tools
 *   - ESP32-A2DP    https://github.com/pschatzmann/ESP32-A2DP
 *   - codec-helix   https://github.com/pschatzmann/arduino-libhelix
 *   - U8g2          (Gestor de librerias de Arduino)
 */

#include <Wire.h>
#include <U8g2lib.h>
#include "AudioTools.h"
#include "AudioTools/Communication/A2DPStream.h"
#include "AudioTools/Disk/AudioSourceSD.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

// ---------------- Configuracion ----------------

// Se conecta al primer dispositivo cuyo nombre EMPIECE con este texto,
// asi "AirPods de Thalia" o "AirPods Pro" tambien sirven.
const char *BT_DEVICE_NAME = "AirPods";

const float VOLUMEN_INICIAL = 0.5;  // 0.0 a 1.0

// Pines (ver README.md)
const int PIN_BTN_ANTERIOR = 13;
const int PIN_BTN_PLAY = 14;
const int PIN_BTN_SIGUIENTE = 27;
const int PIN_SD_CS = 5;  // SCK=18, MISO=19, MOSI=23 (SPI por defecto)
// Pantalla I2C: SDA=21, SCL=22 (I2C por defecto)

// ---------------- Objetos ----------------

AudioSourceSD source("/", "mp3", PIN_SD_CS);
MP3DecoderHelix decoder;
A2DPStream out;
AudioPlayer player(source, out, decoder);

U8G2_SH1106_128X64_NONAME_F_HW_I2C pantalla(U8G2_R0, U8X8_PIN_NONE);

// ---------------- Estado ----------------

bool pausado = false;
bool sdOk = false;
bool estabaConectado = false;
long totalCanciones = 0;
char titulo[96] = "";
char artista[64] = "";
bool redibujar = true;
int scrollX = 0;
unsigned long ultimoScroll = 0;

// ---------------- Botones ----------------

struct Boton {
  int pin;
  bool estadoEstable;
  bool ultimaLectura;
  unsigned long ultimoCambio;
};

Boton botones[] = {
  { PIN_BTN_ANTERIOR, true, true, 0 },
  { PIN_BTN_PLAY, true, true, 0 },
  { PIN_BTN_SIGUIENTE, true, true, 0 },
};

// Devuelve true una sola vez cuando el boton se presiona (con antirrebote).
bool fuePresionado(Boton &b) {
  bool lectura = digitalRead(b.pin);
  if (lectura != b.ultimaLectura) {
    b.ultimoCambio = millis();
    b.ultimaLectura = lectura;
  }
  if (millis() - b.ultimoCambio > 30 && lectura != b.estadoEstable) {
    b.estadoEstable = lectura;
    return lectura == LOW;  // los botones van a GND con INPUT_PULLUP
  }
  return false;
}

// ---------------- Titulo de la cancion ----------------

// Usa el nombre del archivo sin la extension .mp3 como titulo inicial.
void tituloDesdeArchivo() {
  const char *nombre = source.toStr();
  if (nombre == nullptr) nombre = "";
  const char *barra = strrchr(nombre, '/');
  if (barra != nullptr) nombre = barra + 1;
  strncpy(titulo, nombre, sizeof(titulo) - 1);
  titulo[sizeof(titulo) - 1] = '\0';
  char *punto = strrchr(titulo, '.');
  if (punto != nullptr && strcasecmp(punto, ".mp3") == 0) *punto = '\0';
  artista[0] = '\0';
}

// Se llama cada vez que el reproductor abre otra cancion.
void alCambiarCancion(Stream *stream, void *ref) {
  tituloDesdeArchivo();
  scrollX = 0;
  redibujar = true;
}

// Si el MP3 trae etiquetas ID3, se usan en lugar del nombre del archivo.
void alLeerMetadatos(MetaDataType tipo, const char *texto, int len) {
  if (len <= 0) return;
  if (tipo == Title) {
    snprintf(titulo, sizeof(titulo), "%.*s", len, texto);
    scrollX = 0;
    redibujar = true;
  } else if (tipo == Artist) {
    snprintf(artista, sizeof(artista), "%.*s", len, texto);
    redibujar = true;
  }
}

// ---------------- Pantalla ----------------

void dibujarIconoPlay(int x, int y) {
  pantalla.drawTriangle(x, y, x, y + 8, x + 7, y + 4);
}

void dibujarIconoPausa(int x, int y) {
  pantalla.drawBox(x, y, 3, 9);
  pantalla.drawBox(x + 5, y, 3, 9);
}

void dibujarPantalla() {
  pantalla.clearBuffer();

  // Barra superior: estado del Bluetooth
  pantalla.setFont(u8g2_font_6x10_tf);
  if (!sdOk) {
    pantalla.drawUTF8(0, 10, "Error: microSD");
  } else if (!out.isConnected()) {
    pantalla.drawUTF8(0, 10, "Buscando ");
    pantalla.drawUTF8(54, 10, BT_DEVICE_NAME);
  } else {
    pantalla.drawUTF8(0, 10, "BT conectado");
  }
  pantalla.drawHLine(0, 13, 128);

  if (sdOk && totalCanciones == 0) {
    pantalla.drawUTF8(0, 34, "No hay .mp3 en la SD");
  } else if (sdOk) {
    // Titulo (se desplaza si no cabe)
    pantalla.setFont(u8g2_font_7x14_tf);
    int ancho = pantalla.getUTF8Width(titulo);
    if (ancho <= 128) {
      pantalla.drawUTF8(0, 32, titulo);
    } else {
      pantalla.drawUTF8(-scrollX, 32, titulo);
      pantalla.drawUTF8(-scrollX + ancho + 24, 32, titulo);
    }

    // Artista
    pantalla.setFont(u8g2_font_6x10_tf);
    pantalla.drawUTF8(0, 46, artista);

    // Barra inferior: play/pausa y numero de cancion
    if (pausado) {
      dibujarIconoPausa(0, 55);
    } else {
      dibujarIconoPlay(0, 55);
    }
    char pista[16];
    snprintf(pista, sizeof(pista), "%d/%ld", source.index() + 1, totalCanciones);
    pantalla.drawUTF8(128 - pantalla.getUTF8Width(pista), 63, pista);
  }

  pantalla.sendBuffer();
}

// Avanza el texto del titulo cuando es mas ancho que la pantalla.
void actualizarScroll() {
  if (millis() - ultimoScroll < 120) return;
  ultimoScroll = millis();
  pantalla.setFont(u8g2_font_7x14_tf);
  int ancho = pantalla.getUTF8Width(titulo);
  if (ancho <= 128) return;
  scrollX = (scrollX + 3) % (ancho + 24);
  redibujar = true;
}

// ---------------- Controles ----------------

void cancionAnterior() {
  if (totalCanciones == 0) return;
  if (source.index() == 0) {
    player.setIndex(totalCanciones - 1);  // de la primera salta a la ultima
  } else {
    player.previous();
  }
  if (pausado) player.stop();
  redibujar = true;
}

void cancionSiguiente() {
  if (totalCanciones == 0) return;
  if (source.index() >= totalCanciones - 1) {
    player.setIndex(0);  // de la ultima vuelve a la primera
  } else {
    player.next();
  }
  if (pausado) player.stop();
  redibujar = true;
}

void playPausa() {
  pausado = !pausado;
  if (pausado) {
    player.stop();
  } else {
    player.play();
  }
  redibujar = true;
}

// ---------------- Setup y loop ----------------

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  for (Boton &b : botones) pinMode(b.pin, INPUT_PULLUP);

  pantalla.begin();
  pantalla.setBusClock(400000);
  pantalla.enableUTF8Print();
  pantalla.clearBuffer();
  pantalla.setFont(u8g2_font_7x14_tf);
  pantalla.drawUTF8(20, 36, "MP3 casero");
  pantalla.sendBuffer();

  // Bluetooth: no bloquea, sigue buscando los audifonos en segundo plano
  auto cfg = out.defaultConfig(TX_MODE);
  cfg.name = BT_DEVICE_NAME;
  cfg.auto_reconnect = true;
  cfg.wait_for_connection = false;
  cfg.silence_on_nodata = true;  // manda silencio mientras esta en pausa
  out.begin(cfg);

  // Reproductor
  player.setOnStreamChangeCallback(alCambiarCancion);
  player.setMetadataCallback(alLeerMetadatos);
  player.setVolume(VOLUMEN_INICIAL);
  sdOk = player.begin();
  if (sdOk) {
    totalCanciones = source.size();
    Serial.printf("Canciones encontradas: %ld\n", totalCanciones);
  } else {
    Serial.println("No se pudo leer la microSD");
  }

  dibujarPantalla();
}

void loop() {
  bool conectado = out.isConnected();
  if (conectado != estabaConectado) {
    estabaConectado = conectado;
    redibujar = true;
  }

  if (fuePresionado(botones[0])) cancionAnterior();
  if (fuePresionado(botones[1])) playPausa();
  if (fuePresionado(botones[2])) cancionSiguiente();

  // Solo decodificamos cuando los audifonos estan conectados
  if (sdOk && conectado && !pausado) {
    player.copy();
    // Si se termino la lista, vuelve a empezar
    if (!player.isActive() && totalCanciones > 0) {
      player.setIndex(0);
    }
  }

  actualizarScroll();
  if (redibujar) {
    redibujar = false;
    dibujarPantalla();
  }
}
