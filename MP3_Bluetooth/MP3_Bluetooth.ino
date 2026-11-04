/*
 * MP3 casero - ESP32-WROOM
 *
 * Lee los .mp3 y .wav de la microSD y los reproduce por Bluetooth (A2DP) o
 * por la salida con cable (modulo DAC PCM5102A por I2S, con jack de audio).
 *
 * Menu principal (se maneja con el encoder KY-040):
 *   Girar: elegir "Bluetooth" o "Audifono con cable"
 *   Presionar el encoder: entrar
 *
 * Lista de dispositivos Bluetooth:
 *   Girar: moverse por la lista ("< Volver" regresa al menu principal)
 *   Presionar el encoder: conectar al dispositivo elegido
 *
 * Mientras suena la musica:
 *   Girar el encoder: subir / bajar volumen
 *   Presionar el encoder: volver al menu principal
 *   Boton 1 (D13): cancion anterior
 *   Boton 2 (D14): pausa / play
 *   Boton 3 (D27): cancion siguiente
 *
 * En los menus, los botones tambien sirven: 1 sube, 2 elige, 3 baja.
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
#include "esp_gap_bt_api.h"

// ---------------- Configuracion ----------------

const float VOLUMEN_INICIAL = 0.5;  // 0.0 a 1.0
const float PASO_VOLUMEN = 0.05;    // cuanto cambia el volumen por cada clic del encoder

// El KY-040 da 4 pasos por cada "clic" que se siente al girarlo. Si tu encoder
// avanza de dos en dos, cambia esto a 2. Si gira al reves, cambia SENTIDO a -1.
const int PASOS_POR_CLIC = 4;
const int SENTIDO_ENCODER = 1;

// Pines (ver README.md)
const int PIN_BTN_ANTERIOR = 13;
const int PIN_BTN_PLAY = 14;
const int PIN_BTN_SIGUIENTE = 27;
const int PIN_ENC_CLK = 32;
const int PIN_ENC_DT = 33;
const int PIN_ENC_SW = 4;
const int PIN_SD_CS = 5;  // SCK=18, MISO=19, MOSI=23 (SPI por defecto)
// Pantalla I2C: SDA=21, SCL=22 (I2C por defecto)
const int PIN_I2S_BCK = 26;  // PCM5102A: BCK
const int PIN_I2S_LCK = 25;  // PCM5102A: LCK (LRCK / WS)
const int PIN_I2S_DIN = 16;  // PCM5102A: DIN

// ---------------- Objetos ----------------

// Sin extension fija: el filtro de abajo acepta .mp3 y .wav
AudioSourceSD source("/", "", PIN_SD_CS);
const char *TIPOS_DE_ARCHIVO = "*.mp3|*.MP3|*.Mp3|*.wav|*.WAV|*.Wav";
MP3DecoderHelix decoderMP3;
WAVDecoder decoderWAV;
MultiDecoder decoder;  // detecta si cada archivo es MP3 o WAV
A2DPStream out;          // salida Bluetooth
I2SStream cable;         // salida con cable (modulo PCM5102A por I2S)
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
float volumen = VOLUMEN_INICIAL;
unsigned long volumenVisibleHasta = 0;

enum Estado { MENU_PRINCIPAL, MENU_BT, CONECTANDO, REPRODUCIENDO };
Estado estado = MENU_PRINCIPAL;

// Salida de audio actual
const int SALIDA_NINGUNA = 0;
const int SALIDA_BT = 1;
const int SALIDA_CABLE = 2;
int salida = SALIDA_NINGUNA;

int opcionPrincipal = 0;  // 0 = Bluetooth, 1 = Audifono con cable

// ---------------- Dispositivos Bluetooth ----------------

struct DispositivoBT {
  char nombre[32];
  uint8_t dir[6];
  unsigned long visto;  // ultima vez que se encontro (millis)
};

const int MAX_DISPOSITIVOS = 10;
const int FILAS_MENU = 3;
DispositivoBT dispositivos[MAX_DISPOSITIVOS];
volatile int numDispositivos = 0;
volatile bool hayElegido = false;
uint8_t elegidoDir[6];
char elegidoNombre[32] = "";
portMUX_TYPE candado = portMUX_INITIALIZER_UNLOCKED;

bool btIniciado = false;
unsigned long inicioBT = 0;
bool autoReconexionActiva = false;
int seleccionBT = 0;   // 0 = "< Volver", 1.. = dispositivos
int inicioLista = 0;

const unsigned long OLVIDAR_DISPOSITIVO_MS = 30000;  // desaparece de la lista si no se vuelve a ver
const unsigned long TIEMPO_MAX_CONEXION_MS = 60000;  // si no conecta, vuelve a la lista
const unsigned long REINTENTO_BUSQUEDA_MS = 25000;   // vuelve a buscar el dispositivo elegido
unsigned long inicioConexion = 0;
unsigned long avisoHasta = 0;
unsigned long ultimaLimpieza = 0;
unsigned long ultimoSegundo = 0;
unsigned long ultimoReintento = 0;
unsigned long ultimoInforme = 0;
int ultimosPines = -1;

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
    if (i < numDispositivos) {
      dispositivos[i].visto = millis();  // ya estaba: solo marca que sigue ahi
    } else if (numDispositivos < MAX_DISPOSITIVOS) {
      DispositivoBT &d = dispositivos[i];
      d.visto = millis();
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

void iniciarBluetooth() {
  if (btIniciado) return;
  auto cfg = out.defaultConfig(TX_MODE);
  cfg.name = "MP3";  // se ignora: la conexion la decide alEncontrarDispositivo
  cfg.auto_reconnect = false;  // se activa despues de la primera conexion
  cfg.wait_for_connection = false;
  cfg.silence_on_nodata = true;  // manda silencio mientras esta en pausa o en el menu
  out.begin(cfg);
  btIniciado = true;
  inicioBT = millis();
  Serial.println("Bluetooth iniciado: buscando dispositivos");
}

// Si un intento de conexion falla, la libreria se queda quieta y ya no busca.
// Esto arranca otra busqueda para que pueda volver a encontrar el dispositivo.
void asegurarBusqueda() {
  if (!btIniciado || millis() - inicioBT < 15000) return;  // al inicio la libreria busca sola
  if (!out.source().is_discovery_active()) {
    Serial.println("Reiniciando la busqueda Bluetooth");
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  }
}

void ajustarVentanaMenu() {
  if (seleccionBT < inicioLista) inicioLista = seleccionBT;
  if (seleccionBT >= inicioLista + FILAS_MENU) inicioLista = seleccionBT - FILAS_MENU + 1;
}

// Quita de la lista los dispositivos que ya no se ven (por ejemplo, unos
// audifonos que salieron del modo de emparejamiento). Conserva lo que
// tienes seleccionado.
void limpiarDispositivosViejos() {
  uint8_t dirSeleccion[6];
  bool habiaSeleccion = seleccionBT >= 1 && seleccionBT - 1 < numDispositivos;
  if (habiaSeleccion) memcpy(dirSeleccion, dispositivos[seleccionBT - 1].dir, 6);

  bool cambio = false;
  portENTER_CRITICAL(&candado);
  int j = 0;
  for (int i = 0; i < numDispositivos; i++) {
    if (millis() - dispositivos[i].visto <= OLVIDAR_DISPOSITIVO_MS) {
      if (j != i) dispositivos[j] = dispositivos[i];
      j++;
    }
  }
  if (j != numDispositivos) {
    numDispositivos = j;
    cambio = true;
  }
  portEXIT_CRITICAL(&candado);
  if (!cambio) return;

  if (habiaSeleccion) {
    int nueva = 0;  // si desaparecio, vuelve a "< Volver"
    for (int i = 0; i < numDispositivos; i++) {
      if (memcmp(dispositivos[i].dir, dirSeleccion, 6) == 0) nueva = i + 1;
    }
    seleccionBT = nueva;
  }
  if (seleccionBT > numDispositivos) seleccionBT = numDispositivos;
  inicioLista = 0;
  ajustarVentanaMenu();
  redibujar = true;
}

void vaciarListaBT() {
  portENTER_CRITICAL(&candado);
  hayElegido = false;
  numDispositivos = 0;
  portEXIT_CRITICAL(&candado);
  seleccionBT = 0;
  inicioLista = 0;
}

// ---------------- Salida de audio ----------------

// Cambia por donde sale el sonido. nueva: SALIDA_NINGUNA, SALIDA_BT o SALIDA_CABLE
void cambiarSalida(int nueva) {
  if (nueva == salida) return;
  if (salida == SALIDA_CABLE) cable.end();
  if (nueva == SALIDA_CABLE) {
    auto cfg = cable.defaultConfig(TX_MODE);
    cfg.sample_rate = 44100;
    cfg.channels = 2;
    cfg.bits_per_sample = 16;
    cfg.pin_bck = PIN_I2S_BCK;
    cfg.pin_ws = PIN_I2S_LCK;
    cfg.pin_data = PIN_I2S_DIN;
    cfg.pin_mck = -1;  // el PCM5102A no necesita reloj maestro (SCK va a GND)
    cable.begin(cfg);
    player.setOutput(cable);
  } else if (nueva == SALIDA_BT) {
    player.setOutput(out);
  }
  salida = nueva;
  Serial.printf("Salida de audio: %s\n",
                nueva == SALIDA_BT ? "Bluetooth" : (nueva == SALIDA_CABLE ? "cable" : "ninguna"));
}

// ---------------- Navegacion ----------------

void irAlMenuPrincipal() {
  // El cable se apaga para que no zumbe mientras estas en el menu
  if (salida == SALIDA_CABLE) cambiarSalida(SALIDA_NINGUNA);
  estado = MENU_PRINCIPAL;
  redibujar = true;
}

void irAReproducir() {
  estado = REPRODUCIENDO;
  scrollX = 0;
  redibujar = true;
}

void elegirOpcionPrincipal() {
  if (opcionPrincipal == 0) {
    // Bluetooth
    iniciarBluetooth();
    if (out.isConnected()) {
      cambiarSalida(SALIDA_BT);  // ya estaba conectado: directo a las canciones
      irAReproducir();
    } else {
      estado = MENU_BT;
      redibujar = true;
    }
  } else {
    // Audifono con cable
    cambiarSalida(SALIDA_CABLE);
    irAReproducir();
  }
}

void moverMenuBT(int pasos) {
  int total = numDispositivos + 1;  // + "< Volver"
  seleccionBT = ((seleccionBT + pasos) % total + total) % total;
  ajustarVentanaMenu();
  redibujar = true;
}

void elegirEnMenuBT() {
  if (seleccionBT == 0 || seleccionBT - 1 >= numDispositivos) {
    irAlMenuPrincipal();
    return;
  }
  int i = seleccionBT - 1;
  portENTER_CRITICAL(&candado);
  memcpy(elegidoDir, dispositivos[i].dir, 6);
  strncpy(elegidoNombre, dispositivos[i].nombre, sizeof(elegidoNombre) - 1);
  elegidoNombre[sizeof(elegidoNombre) - 1] = '\0';
  hayElegido = true;
  portEXIT_CRITICAL(&candado);
  estado = CONECTANDO;
  inicioConexion = millis();
  ultimoReintento = millis();
  redibujar = true;
  Serial.printf("Conectando a %s (%02X:%02X:%02X:%02X:%02X:%02X)\n", elegidoNombre,
                elegidoDir[0], elegidoDir[1], elegidoDir[2], elegidoDir[3], elegidoDir[4], elegidoDir[5]);
}

void cancelarConexion() {
  portENTER_CRITICAL(&candado);
  hayElegido = false;
  portEXIT_CRITICAL(&candado);
  estado = MENU_BT;
  redibujar = true;
}

void cambiarVolumen(int pasos) {
  volumen += pasos * PASO_VOLUMEN;
  if (volumen < 0) volumen = 0;
  if (volumen > 1) volumen = 1;
  player.setVolume(volumen);
  volumenVisibleHasta = millis() + 1500;
  redibujar = true;
}

// ---------------- Encoder KY-040 ----------------

volatile int pasosEncoder = 0;
volatile uint8_t estadoEncoder = 0;
portMUX_TYPE candadoEncoder = portMUX_INITIALIZER_UNLOCKED;

// Se ejecuta cada vez que cambia CLK o DT. Usa una tabla para saber si el
// cambio fue hacia un lado o hacia el otro (e ignora rebotes).
void IRAM_ATTR alGirarEncoder() {
  static const int8_t tabla[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
  portENTER_CRITICAL_ISR(&candadoEncoder);
  estadoEncoder = ((estadoEncoder << 2) | (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT)) & 0x0F;
  pasosEncoder += tabla[estadoEncoder];
  portEXIT_CRITICAL_ISR(&candadoEncoder);
}

// Devuelve cuantos clics se giro desde la ultima vez (positivo o negativo).
int leerGiro() {
  portENTER_CRITICAL(&candadoEncoder);
  int pasos = pasosEncoder;
  int clics = pasos / PASOS_POR_CLIC;
  pasosEncoder = pasos - clics * PASOS_POR_CLIC;
  portEXIT_CRITICAL(&candadoEncoder);
  return clics * SENTIDO_ENCODER;
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
  { PIN_ENC_SW, true, true, 0 },  // boton del encoder
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

// Usa el nombre del archivo sin la extension como titulo inicial.
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

void dibujarEncabezado(const char *texto) {
  pantalla.setFont(u8g2_font_6x10_tf);
  pantalla.drawUTF8(0, 10, texto);
  pantalla.drawHLine(0, 13, 128);
}

void dibujarMenuPrincipal() {
  dibujarEncabezado("Menu principal");
  const char *opciones[2] = { "Bluetooth", "Audifono con cable" };
  for (int i = 0; i < 2; i++) {
    int y = 30 + i * 14;
    if (i == opcionPrincipal) {
      pantalla.drawBox(0, y - 10, 128, 13);
      pantalla.setDrawColor(0);
    }
    char linea[28];
    if (i == 0 && btIniciado && out.isConnected()) {
      snprintf(linea, sizeof(linea), " %s: conectado", opciones[i]);
    } else {
      snprintf(linea, sizeof(linea), " %s", opciones[i]);
    }
    pantalla.drawUTF8(0, y, linea);
    pantalla.setDrawColor(1);
  }
  pantalla.drawUTF8(0, 63, "Gira y presiona");
}

void dibujarMenuBT() {
  dibujarEncabezado("Dispositivos BT");
  int n = numDispositivos;
  int total = n + 1;
  for (int fila = 0; fila < FILAS_MENU; fila++) {
    int i = inicioLista + fila;
    if (i >= total) break;
    int y = 25 + fila * 12;
    if (i == seleccionBT) pantalla.drawUTF8(0, y, ">");
    char linea[24];
    if (i == 0) {
      snprintf(linea, sizeof(linea), "< Volver");
    } else {
      snprintf(linea, sizeof(linea), "%.19s", dispositivos[i - 1].nombre);
    }
    pantalla.drawUTF8(8, y, linea);
  }
  if (millis() < avisoHasta) {
    pantalla.drawUTF8(0, 63, "No se pudo conectar");
  } else if (n == 0) {
    pantalla.drawUTF8(0, 63, "Buscando...");
  } else {
    pantalla.drawUTF8(0, 63, "Gira y presiona");
  }
}

void dibujarConectando() {
  dibujarEncabezado("Conectando a:");
  char linea[24];
  snprintf(linea, sizeof(linea), "%.20s", elegidoNombre);
  pantalla.drawUTF8(0, 32, linea);
  char espera[28];
  snprintf(espera, sizeof(espera), "%s %lus",
           out.source().is_discovery_active() ? "Buscando" : "Enlazando",
           (millis() - inicioConexion) / 1000);
  pantalla.drawUTF8(0, 46, espera);
  pantalla.drawUTF8(0, 63, "Presiona: cancelar");
}

void dibujarReproduciendo() {
  pantalla.setFont(u8g2_font_6x10_tf);
  char barra[24];
  if (salida == SALIDA_BT) {
    snprintf(barra, sizeof(barra), "BT: %.16s", elegidoNombre);
  } else {
    snprintf(barra, sizeof(barra), "Cable");
  }
  dibujarEncabezado(barra);

  if (totalCanciones == 0) {
    pantalla.drawUTF8(0, 34, "No hay canciones");
    return;
  }

  // Titulo (se desplaza si no cabe)
  pantalla.setFont(u8g2_font_7x14_tf);
  int ancho = pantalla.getUTF8Width(titulo);
  if (ancho <= 128) {
    pantalla.drawUTF8(0, 32, titulo);
  } else {
    pantalla.drawUTF8(-scrollX, 32, titulo);
    pantalla.drawUTF8(-scrollX + ancho + 24, 32, titulo);
  }

  // Artista, o la barra de volumen justo despues de girar el encoder
  pantalla.setFont(u8g2_font_6x10_tf);
  if (millis() < volumenVisibleHasta) {
    pantalla.drawUTF8(0, 46, "Vol");
    pantalla.drawFrame(22, 38, 100, 8);
    pantalla.drawBox(22, 38, (int)(volumen * 100 + 0.5), 8);
  } else {
    pantalla.drawUTF8(0, 46, artista);
  }

  // Barra inferior: play/pausa, volumen y numero de cancion
  if (pausado) {
    dibujarIconoPausa(0, 55);
  } else {
    dibujarIconoPlay(0, 55);
  }
  char vol[12];
  snprintf(vol, sizeof(vol), "%d%%", (int)(volumen * 100 + 0.5));
  pantalla.drawUTF8(14, 63, vol);
  char pista[16];
  snprintf(pista, sizeof(pista), "%d/%ld", source.index() + 1, totalCanciones);
  pantalla.drawUTF8(128 - pantalla.getUTF8Width(pista), 63, pista);
}

void dibujarPantalla() {
  pantalla.clearBuffer();
  if (!sdOk) {
    dibujarEncabezado("Error microSD:");
    pantalla.drawUTF8(0, 30, errorSD);
  } else if (estado == MENU_PRINCIPAL) {
    dibujarMenuPrincipal();
  } else if (estado == MENU_BT) {
    dibujarMenuBT();
  } else if (estado == CONECTANDO) {
    dibujarConectando();
  } else {
    dibujarReproduciendo();
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

// ---------------- Controles de reproduccion ----------------

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
  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  estadoEncoder = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), alGirarEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), alGirarEncoder, CHANGE);

  pantalla.begin();
  pantalla.setBusClock(400000);
  pantalla.enableUTF8Print();
  pantalla.clearBuffer();
  pantalla.setFont(u8g2_font_7x14_tf);
  pantalla.drawUTF8(20, 36, "MP3 casero");
  pantalla.sendBuffer();

  // Bluetooth: se prepara aqui, pero solo empieza a buscar cuando eliges
  // "Bluetooth" en el menu principal.
  out.source().set_ssid_callback(alEncontrarDispositivo);
  out.source().set_ssp_enabled(true);  // los AirPods y otros exigen emparejamiento seguro (SSP)

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
    player.setVolume(volumen);
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
  bool conectado = btIniciado && out.isConnected();

  // ----- Cambios de conexion Bluetooth -----
  if (estado == CONECTANDO && conectado) {
    cambiarSalida(SALIDA_BT);
    irAReproducir();
    if (!autoReconexionActiva) {
      // A partir de ahora, si se corta la conexion intenta volver sola
      out.source().set_auto_reconnect(true, 3);
      autoReconexionActiva = true;
    }
  } else if (estado == REPRODUCIENDO && salida == SALIDA_BT && !conectado) {
    estado = CONECTANDO;  // se corto: muestra "Conectando" mientras reintenta
    inicioConexion = millis();
    redibujar = true;
  }

  // ----- Mantenimiento de la lista y de la conexion -----
  if (estado == MENU_BT && millis() - ultimaLimpieza > 1000) {
    ultimaLimpieza = millis();
    limpiarDispositivosViejos();
    if (millis() - ultimoReintento > 3000) {
      ultimoReintento = millis();
      asegurarBusqueda();
    }
  }
  if (estado == CONECTANDO) {
    if (!autoReconexionActiva && millis() - inicioConexion > TIEMPO_MAX_CONEXION_MS) {
      // No conecto: vuelve a la lista vacia para buscar de nuevo
      Serial.println("No se pudo conectar: vuelve a la lista");
      vaciarListaBT();
      estado = MENU_BT;
      avisoHasta = millis() + 5000;
      redibujar = true;
    } else if (!autoReconexionActiva && millis() - ultimoReintento > REINTENTO_BUSQUEDA_MS) {
      ultimoReintento = millis();
      asegurarBusqueda();
    } else if (millis() - ultimoSegundo >= 1000) {
      ultimoSegundo = millis();
      redibujar = true;  // actualiza el contador de segundos
    }
  }

  // ----- Diagnostico en el Monitor Serie -----
  int pines = digitalRead(PIN_BTN_ANTERIOR) | (digitalRead(PIN_BTN_PLAY) << 1) |
              (digitalRead(PIN_BTN_SIGUIENTE) << 2) | (digitalRead(PIN_ENC_SW) << 3);
  if (pines != ultimosPines) {
    ultimosPines = pines;  // 1 = suelto, 0 = presionado (conectado a GND)
    Serial.printf("Pines: D13=%d D14=%d D27=%d SW(D4)=%d\n",
                  pines & 1, (pines >> 1) & 1, (pines >> 2) & 1, (pines >> 3) & 1);
  }
  if (millis() - ultimoInforme > 5000) {
    ultimoInforme = millis();
    Serial.printf("[estado=%d salida=%d dispositivos=%d buscando=%d conectado=%d vol=%d%%]\n",
                  (int)estado, salida, (int)numDispositivos,
                  btIniciado ? (int)out.source().is_discovery_active() : 0, (int)conectado,
                  (int)(volumen * 100 + 0.5));
  }

  // ----- Entradas: encoder y botones -----
  int giro = leerGiro();
  bool b1 = fuePresionado(0);
  bool b2 = fuePresionado(1);
  bool b3 = fuePresionado(2);
  bool clic = fuePresionado(3);
  if (giro != 0) Serial.printf("Encoder: %d\n", giro);
  if (b1 || b2 || b3 || clic) {
    Serial.printf("Boton %s (estado %d)\n", clic ? "encoder" : (b1 ? "1" : (b2 ? "2" : "3")), (int)estado);
  }

  if (!sdOk) {
    // sin microSD no hay nada que hacer
  } else if (estado == MENU_PRINCIPAL) {
    int mover = giro + (b3 ? 1 : 0) - (b1 ? 1 : 0);
    if (mover != 0) {
      opcionPrincipal = ((opcionPrincipal + mover) % 2 + 2) % 2;
      redibujar = true;
    }
    if (clic || b2) elegirOpcionPrincipal();
  } else if (estado == MENU_BT) {
    int mover = giro + (b3 ? 1 : 0) - (b1 ? 1 : 0);
    if (mover != 0) moverMenuBT(mover);
    if (clic || b2) elegirEnMenuBT();
  } else if (estado == CONECTANDO) {
    if (clic || b1) {
      if (autoReconexionActiva) {
        irAlMenuPrincipal();  // se habia cortado: vuelve al menu principal
      } else {
        cancelarConexion();   // primera conexion: vuelve a la lista
      }
    }
  } else {
    if (giro != 0) cambiarVolumen(giro);
    if (clic) irAlMenuPrincipal();
    if (b1) cancionAnterior();
    if (b2) playPausa();
    if (b3) cancionSiguiente();
  }

  // ----- Audio -----
  // Solo se decodifica mientras estas en la pantalla de canciones
  if (sdOk && estado == REPRODUCIENDO && !pausado) {
    player.copy();
    // Si se termino la lista, vuelve a empezar
    if (!player.isActive() && totalCanciones > 0) {
      player.setIndex(0);
    }
  }

  // ----- Pantalla -----
  actualizarScroll();
  if (volumenVisibleHasta != 0 && millis() >= volumenVisibleHasta) {
    volumenVisibleHasta = 0;
    redibujar = true;  // quita la barra de volumen
  }
  if (redibujar) {
    redibujar = false;
    dibujarPantalla();
  }
}
