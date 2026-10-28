/*
 * MP3 casero - ESP32-WROOM
 *
 * Al encender busca dispositivos Bluetooth de audio y los muestra en la
 * pantalla. Tu eliges a cual conectarte y hasta entonces empieza la musica.
 * Lee los .mp3 y .wav de la microSD y los envia por Bluetooth (A2DP).
 *
 * Menu de dispositivos:
 *   Boton 1 (D13): subir
 *   Boton 2 (D14): elegir
 *   Boton 3 (D27): bajar
 *
 * Mientras suena la musica:
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
#include "AudioTools/AudioCodecs/CodecWAV.h"
#include "AudioTools/AudioCodecs/MultiDecoder.h"

// ---------------- Configuracion ----------------

const float VOLUMEN_INICIAL = 0.5;  // 0.0 a 1.0

// Pines (ver README.md)
const int PIN_BTN_ANTERIOR = 13;
const int PIN_BTN_PLAY = 14;
const int PIN_BTN_SIGUIENTE = 27;
const int PIN_SD_CS = 5;  // SCK=18, MISO=19, MOSI=23 (SPI por defecto)
// Pantalla I2C: SDA=21, SCL=22 (I2C por defecto)

// ---------------- Objetos ----------------

// Sin extension fija: el filtro de abajo acepta .mp3 y .wav
AudioSourceSD source("/", "", PIN_SD_CS);
const char *TIPOS_DE_ARCHIVO = "*.mp3|*.MP3|*.Mp3|*.wav|*.WAV|*.Wav";
MP3DecoderHelix decoderMP3;
WAVDecoder decoderWAV;
MultiDecoder decoder;  // detecta si cada archivo es MP3 o WAV
A2DPStream out;
AudioPlayer player(source, out, decoder);

U8G2_SH1106_128X64_NONAME_F_HW_I2C pantalla(U8G2_R0, U8X8_PIN_NONE);

// ---------------- Estado ----------------

bool pausado = false;
bool sdOk = false;
const char *errorSD = "";  // texto del error de la microSD para la pantalla
long totalCanciones = 0;
char titulo[96] = "";
char artista[64] = "";
volatile bool redibujar = true;
int scrollX = 0;
unsigned long ultimoScroll = 0;

// ---------------- Dispositivos Bluetooth ----------------

struct DispositivoBT {
  char nombre[32];
  uint8_t dir[6];
};

const int MAX_DISPOSITIVOS = 10;
const int FILAS_MENU = 3;
DispositivoBT dispositivos[MAX_DISPOSITIVOS];
volatile int numDispositivos = 0;
volatile bool hayElegido = false;
uint8_t elegidoDir[6];
char elegidoNombre[32] = "";
portMUX_TYPE candado = portMUX_INITIALIZER_UNLOCKED;

enum Estado { MENU, CONECTANDO, REPRODUCIENDO };
Estado estado = MENU;
int seleccion = 0;
int inicioLista = 0;
bool autoReconexionActiva = false;

// La libreria llama a esta funcion por cada dispositivo de audio que encuentra.
// Mientras no hayas elegido, solo los guarda en la lista (y devuelve false para
// seguir buscando). Cuando ya elegiste, devuelve true solo para ese dispositivo.
bool alEncontrarDispositivo(const char *nombre, esp_bd_addr_t dir, int rssi) {
  bool conectar = false;
  portENTER_CRITICAL(&candado);
  if (hayElegido) {
    conectar = memcmp(dir, elegidoDir, 6) == 0;
  } else {
    int i = 0;
    while (i < numDispositivos && memcmp(dispositivos[i].dir, dir, 6) != 0) i++;
    if (i == numDispositivos && numDispositivos < MAX_DISPOSITIVOS) {
      DispositivoBT &d = dispositivos[i];
      memcpy(d.dir, dir, 6);
      if (nombre != nullptr && nombre[0] != '\0') {
        strncpy(d.nombre, nombre, sizeof(d.nombre) - 1);
        d.nombre[sizeof(d.nombre) - 1] = '\0';
      } else {
        snprintf(d.nombre, sizeof(d.nombre), "Dispositivo %02X%02X", dir[4], dir[5]);
      }
      numDispositivos = numDispositivos + 1;
      redibujar = true;
    }
  }
  portEXIT_CRITICAL(&candado);
  return conectar;
}

void ajustarVentanaMenu() {
  if (seleccion < inicioLista) inicioLista = seleccion;
  if (seleccion >= inicioLista + FILAS_MENU) inicioLista = seleccion - FILAS_MENU + 1;
}

void menuSubir() {
  int n = numDispositivos;
  if (n == 0) return;
  seleccion = (seleccion - 1 + n) % n;
  ajustarVentanaMenu();
  redibujar = true;
}

void menuBajar() {
  int n = numDispositivos;
  if (n == 0) return;
  seleccion = (seleccion + 1) % n;
  ajustarVentanaMenu();
  redibujar = true;
}

void menuElegir() {
  if (numDispositivos == 0) return;
  portENTER_CRITICAL(&candado);
  memcpy(elegidoDir, dispositivos[seleccion].dir, 6);
  strncpy(elegidoNombre, dispositivos[seleccion].nombre, sizeof(elegidoNombre) - 1);
  elegidoNombre[sizeof(elegidoNombre) - 1] = '\0';
  hayElegido = true;
  portEXIT_CRITICAL(&candado);
  estado = CONECTANDO;
  redibujar = true;
}

void volverAlMenu() {
  portENTER_CRITICAL(&candado);
  hayElegido = false;
  portEXIT_CRITICAL(&candado);
  estado = MENU;
  redibujar = true;
}

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
bool fuePresionado(int numero) {
  Boton &b = botones[numero];
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
  if (punto != nullptr && (strcasecmp(punto, ".mp3") == 0 || strcasecmp(punto, ".wav") == 0)) *punto = '\0';
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

void dibujarMenu() {
  pantalla.setFont(u8g2_font_6x10_tf);
  pantalla.drawUTF8(0, 10, "Dispositivos BT");
  pantalla.drawHLine(0, 13, 128);
  int n = numDispositivos;
  if (n == 0) {
    pantalla.drawUTF8(0, 30, "Buscando...");
    pantalla.drawUTF8(0, 44, "Pon tus audifonos en");
    pantalla.drawUTF8(0, 56, "modo emparejamiento");
    return;
  }
  for (int fila = 0; fila < FILAS_MENU; fila++) {
    int i = inicioLista + fila;
    if (i >= n) break;
    int y = 25 + fila * 12;
    if (i == seleccion) pantalla.drawUTF8(0, y, ">");
    char linea[24];
    snprintf(linea, sizeof(linea), "%.19s", dispositivos[i].nombre);
    pantalla.drawUTF8(8, y, linea);
  }
  pantalla.drawUTF8(0, 63, "1:sube 2:OK 3:baja");
}

void dibujarConectando() {
  pantalla.setFont(u8g2_font_6x10_tf);
  pantalla.drawUTF8(0, 10, "Conectando a:");
  pantalla.drawHLine(0, 13, 128);
  char linea[24];
  snprintf(linea, sizeof(linea), "%.20s", elegidoNombre);
  pantalla.drawUTF8(0, 32, linea);
  pantalla.drawUTF8(0, 46, "Espera un momento...");
  pantalla.drawUTF8(0, 63, "1: volver a la lista");
}

void dibujarPantalla() {
  pantalla.clearBuffer();

  if (!sdOk) {
    pantalla.setFont(u8g2_font_6x10_tf);
    pantalla.drawUTF8(0, 10, "Error microSD:");
    pantalla.drawHLine(0, 13, 128);
    pantalla.drawUTF8(0, 30, errorSD);
    pantalla.sendBuffer();
    return;
  }
  if (estado == MENU) {
    dibujarMenu();
    pantalla.sendBuffer();
    return;
  }
  if (estado == CONECTANDO) {
    dibujarConectando();
    pantalla.sendBuffer();
    return;
  }

  // Barra superior: dispositivo conectado
  pantalla.setFont(u8g2_font_6x10_tf);
  char barra[24];
  snprintf(barra, sizeof(barra), "BT: %.16s", elegidoNombre);
  pantalla.drawUTF8(0, 10, barra);
  pantalla.drawHLine(0, 13, 128);

  if (totalCanciones == 0) {
    pantalla.drawUTF8(0, 34, "No hay canciones");
  } else {
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
  if (estado != REPRODUCIENDO) return;
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

  // Bluetooth: no bloquea. Empieza a buscar dispositivos y la funcion
  // alEncontrarDispositivo los va agregando a la lista del menu.
  out.source().set_ssid_callback(alEncontrarDispositivo);
  auto cfg = out.defaultConfig(TX_MODE);
  cfg.name = "MP3";  // se ignora: la conexion la decide alEncontrarDispositivo
  cfg.auto_reconnect = false;  // se activa despues de la primera conexion
  cfg.wait_for_connection = false;
  cfg.silence_on_nodata = true;  // manda silencio mientras esta en pausa
  out.begin(cfg);

  // microSD: se revisa primero para mostrar un error claro
  if (!SD.begin(PIN_SD_CS)) {
    errorSD = "no se detecta";
    Serial.println("microSD: no se detecta. Revisa cables, VCC y formato FAT32");
  } else {
    Serial.printf("microSD detectada: %llu MB\n", SD.cardSize() / (1024 * 1024));

    // Reproductor
    source.setFileFilter(TIPOS_DE_ARCHIVO);
    decoder.addDecoder(decoderMP3, "audio/mpeg");
    decoder.addDecoder(decoderWAV, "audio/vnd.wave");
    player.setOnStreamChangeCallback(alCambiarCancion);
    player.setMetadataCallback(alLeerMetadatos);
    player.setVolume(VOLUMEN_INICIAL);
    sdOk = player.begin();
    totalCanciones = source.size();
    Serial.printf("Canciones encontradas: %ld\n", totalCanciones);
    if (!sdOk) {
      errorSD = totalCanciones == 0 ? "no hay mp3 ni wav" : "no abre la cancion";
      Serial.println("microSD: no se pudo abrir ninguna cancion .mp3 o .wav");
    }
  }

  dibujarPantalla();
}

void loop() {
  bool conectado = out.isConnected();
  if (estado == CONECTANDO && conectado) {
    estado = REPRODUCIENDO;
    redibujar = true;
    if (!autoReconexionActiva) {
      // A partir de ahora, si se corta la conexion intenta volver sola
      out.source().set_auto_reconnect(true, 3);
      autoReconexionActiva = true;
    }
  } else if (estado == REPRODUCIENDO && !conectado) {
    estado = CONECTANDO;  // se corto: muestra "Conectando" mientras reintenta
    redibujar = true;
  }

  bool b1 = fuePresionado(0);
  bool b2 = fuePresionado(1);
  bool b3 = fuePresionado(2);
  if (estado == MENU) {
    if (b1) menuSubir();
    if (b2) menuElegir();
    if (b3) menuBajar();
  } else if (estado == CONECTANDO) {
    if (b1) volverAlMenu();
  } else {
    if (b1) cancionAnterior();
    if (b2) playPausa();
    if (b3) cancionSiguiente();
  }

  // Solo decodificamos cuando los audifonos estan conectados
  if (sdOk && estado == REPRODUCIENDO && !pausado) {
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
