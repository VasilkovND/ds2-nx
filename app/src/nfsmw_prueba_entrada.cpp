// nfsmw - automated tests on the PC (see nfsmw_prueba_entrada.h).

#include "nfsmw_prueba_entrada.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <atomic>
#include <vector>

REXCVAR_DEFINE_STRING(nfsmw_prueba_botones, "", "NFSMW",
                      "Pruebas: guion de botones de un mando virtual, en segundos desde el "
                      "arranque. \"92:start,98:a,130-160:rt\" pulsa START a los 92 s, A a los 98 s y "
                      "mantiene el gatillo derecho de 130 a 160 s. Botones: a b x y start back arriba "
                      "abajo izquierda derecha lb rb lt rt, y de la palanca izquierda "
                      "palanca_arriba palanca_abajo palanca_izquierda palanca_derecha")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Race tests: the car has to move forward following the track without leaving it. The button script only
// holds the trigger, and the car ended up against a wall. The game has a script command "ForceAIControl"
// (registered with sub_8237B858 at 0x8235ED28; handler sub_82367128): with r3 = 0 it takes player 1 from the
// list 0x82C74BA0 (sub_82366FB0), looks up its AI interface (sub_8231AF38) and, unless it is already active,
// enables AI control (virtual function +16 with 1). It is what the game does when crossing the finish line:
// the AI drives the player's car along the racing line.
REXCVAR_DEFINE_BOOL(nfsmw_prueba_ia_conduce, false, "NFSMW",
                    "Pruebas: en cada carrera la IA del juego conduce el coche del jugador desde el principio "
                    "(comando ForceAIControl), para que avance siguiendo el circuito")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Enabled after the countdown: when it was enabled 30 frames after entering the race (during the intro, with
// the script's A presses already inside the race), the car ended up flipped and off the track.
REXCVAR_DEFINE_DOUBLE(nfsmw_prueba_ia_conduce_retraso_s, 12.0, "NFSMW",
                      "Pruebas: segundos desde que empieza la carrera (estado 6) hasta que la IA toma el coche "
                      "del jugador (nfsmw_prueba_ia_conduce)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Alley shortcut in Ironwood Estates: the robotic-audio tests have to go through it, and the game's AI does
// not take it. The command registered right before ForceAIControl (handler sub_823671C0) does the opposite:
// with r3 = 0 it takes player 1, looks up its AI interface and, if the AI is driving, calls virtual function
// +16 with 0 and gives the car back to the player.
REXCVAR_DEFINE_DOUBLE(nfsmw_prueba_ia_suelta_s, 0.0, "NFSMW",
                      "Pruebas: segundos desde que empieza la carrera (estado 6) hasta que se le quita el coche a la "
                      "IA (nfsmw_prueba_ia_conduce); 0 = nunca")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(nfsmw_prueba_tras_soltar, "", "NFSMW",
                      "Pruebas: botones del mando virtual desde que se suelta la IA, en segundos contados desde ese "
                      "momento, con el formato de nfsmw_prueba_botones (\"0-15:rt\" acelera 15 s)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Player car position: the AI does not repeat the race identically (in one run it passed the checkpoint at
// 41.5 s and in another at 53 s, with traffic in between), so releasing it at a fixed time does not bring
// the car to the alley. Steering it requires knowing where it is: these dumps store the interfaces of
// player 1's car (the same ones ForceAIControl uses) and what they point to, to find the position and
// speed offline.
REXCVAR_DEFINE_STRING(nfsmw_prueba_volcar_coche, "", "NFSMW",
                      "Pruebas: segundos desde que empieza la carrera (estado 6) en los que se vuelca la memoria del "
                      "coche del jugador a logs/coche_N.bin, separados por comas (\"30,30.25,30.5,31\"); vacio = "
                      "nunca")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Alley shortcut autopilot: on the home straight the car is taken from the AI and the left stick drives it
// along a route of points (world meters, taken from the trace) with the throttle floored. After the last
// point, or after nfsmw_prueba_piloto_max_s seconds, the AI drives again (ForceAIControl). It rearms when
// the car is 300 m away from the first point, so in a 2-lap race it runs twice. The alley turns left about
// 225 m from the home straight: with a single straight segment the autopilot dragged the car along the
// right-hand wall.
REXCVAR_DEFINE_BOOL(nfsmw_prueba_traza_coche, false, "NFSMW",
                    "Pruebas: guarda en logs/traza_coche.csv la posicion, la velocidad y el rumbo del coche del "
                    "jugador en cada fotograma de carrera")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(nfsmw_prueba_piloto, "", "NFSMW",
                      "Pruebas: ruta \"x1,y1,x2,y2[,x3,y3...]\" (metros del mundo) por la que el piloto lleva el "
                      "coche del jugador cuando pasa a menos de nfsmw_prueba_piloto_radio del primer punto; vacio = "
                      "sin piloto")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_prueba_piloto_radio, 20.0, "NFSMW",
                      "Pruebas: distancia a A (m) a la que el piloto le quita el coche a la IA")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_prueba_piloto_adelanto, 25.0, "NFSMW",
                      "Pruebas: metros por delante, sobre la linea, a los que apunta el piloto")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_prueba_piloto_angulo, 25.0, "NFSMW",
                      "Pruebas: grados de error de rumbo con los que el piloto gira el stick del todo")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_prueba_piloto_signo, 1.0, "NFSMW",
                      "Pruebas: 1 o -1, sentido del stick del piloto")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_prueba_piloto_max_s, 12.0, "NFSMW",
                      "Pruebas: segundos como mucho que conduce el piloto antes de devolverle el coche a la IA")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_prueba_piloto_zona_muerta, 0.24, "NFSMW",
                      "Pruebas: fraccion del stick por debajo de la cual el juego no gira; las correcciones del "
                      "piloto empiezan ahi (0,24 es la zona muerta habitual de XInput)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824411B8);
REX_EXTERN(__imp__sub_82366FB0);
REX_EXTERN(__imp__sub_82367128);
REX_EXTERN(__imp__sub_823671C0);

namespace nfsmw::prueba {
namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using Reloj = std::chrono::steady_clock;
namespace in = rex::input;

constexpr in::DeviceId kMandoGuion = in::DeviceId(0x4E46534D57475549ull);  // "NFSMWGUI"
constexpr double kPulsacion = 0.25;  // seconds of a single press

// X_INPUT_GAMEPAD bits; the triggers go in bits 16 and 17. The key is the
// XInputGetKeystroke VK_PAD code (ui/virtual_key.h).
struct Boton {
  std::string_view nombre;
  uint32_t bits;
  uint16_t tecla;
};
constexpr Boton kBotones[] = {
    {"a", 0x1000, 0x5800},          {"b", 0x2000, 0x5801},
    {"x", 0x4000, 0x5802},          {"y", 0x8000, 0x5803},
    {"rb", 0x0200, 0x5804},         {"lb", 0x0100, 0x5805},
    {"lt", 1u << 16, 0x5806},       {"rt", 1u << 17, 0x5807},
    {"arriba", 0x0001, 0x5810},     {"abajo", 0x0002, 0x5811},
    {"izquierda", 0x0004, 0x5812},  {"derecha", 0x0008, 0x5813},
    {"start", 0x0010, 0x5814},      {"back", 0x0020, 0x5815},
    // The left stick. The "create a profile?" dialog ignores the D-pad and only responds to the stick, so
    // without this there was no way to reach a save from the script. The codes are the real ones
    // (VK_PAD_LTHUMB_UP and following), for the menus that read keystrokes instead of state.
    {"palanca_arriba", 1u << 18, 0x5820},     {"palanca_abajo", 1u << 19, 0x5821},
    {"palanca_derecha", 1u << 20, 0x5822},    {"palanca_izquierda", 1u << 21, 0x5823},
};

struct Paso {
  double inicio;
  double fin;
  uint32_t bits;
};

bool Numero(std::string_view texto, double& valor) {
  const std::string copia(texto);
  char* fin = nullptr;
  valor = std::strtod(copia.c_str(), &fin);
  return !copia.empty() && fin == copia.c_str() + copia.size() && valor >= 0.0;
}

// "T:button" presses briefly; "T1-T2:button" holds. Separated by commas,
// spaces or semicolons.
std::vector<Paso> Leer(std::string_view texto) {
  std::vector<Paso> pasos;
  size_t i = 0;
  const auto separador = [](char c) { return c == ' ' || c == ',' || c == ';'; };
  while (i < texto.size()) {
    while (i < texto.size() && separador(texto[i])) {
      ++i;
    }
    size_t j = i;
    while (j < texto.size() && !separador(texto[j])) {
      ++j;
    }
    if (j == i) {
      break;
    }
    const std::string_view paso = texto.substr(i, j - i);
    i = j;
    const size_t dos_puntos = paso.find(':');
    const std::string_view tiempos = paso.substr(0, dos_puntos);
    const std::string_view nombre =
        dos_puntos == std::string_view::npos ? std::string_view() : paso.substr(dos_puntos + 1);
    const Boton* boton = nullptr;
    for (const Boton& b : kBotones) {
      if (b.nombre == nombre) {
        boton = &b;
      }
    }
    const size_t guion = tiempos.find('-');
    double inicio = 0.0;
    double fin = 0.0;
    const bool bien = boton && Numero(tiempos.substr(0, guion), inicio) &&
                      (guion == std::string_view::npos
                           ? (fin = inicio + kPulsacion, true)
                           : (Numero(tiempos.substr(guion + 1), fin) && fin > inicio));
    if (!bien) {
      REXLOG_WARN("[prueba] mando del guion: paso no valido \"{}\": se ignora", paso);
      continue;
    }
    pasos.push_back({inicio, fin, boton->bits});
  }
  return pasos;
}

// Buttons of nfsmw_prueba_tras_soltar at this instant: the game's main thread writes them (IaConduce) and
// the virtual pad adds them. A value that changes a few times per race, with no waiting between threads.
std::atomic<uint32_t> g_bits_tras_soltar{0};
// Left stick, horizontal axis (negative = left): set by the shortcut autopilot from the main thread.
std::atomic<int16_t> g_stick_lx{0};

class MandoGuion final : public in::InputDriver {
 public:
  explicit MandoGuion(std::vector<Paso> pasos)
      : InputDriver(nullptr, 0), pasos_(std::move(pasos)), inicio_(Reloj::now()) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<in::DeviceInfo>& out) override {
    in::DeviceInfo info;
    info.id = kMandoGuion;
    info.name = "Mando del guion de pruebas";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(in::DeviceId id, in::X_INPUT_STATE* out_state) override {
    if (id != kMandoGuion) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    uint32_t paquete = 0;
    int16_t stick = 0;
    const uint32_t bits = Actualizar(paquete, stick);
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = paquete;
      out_state->gamepad.buttons = uint16_t(bits & 0xFFFF);
      // The script's stick overrides the autopilot while held, which is what the menus need.
      int16_t stick_y = 0;
      if ((bits >> 18) & 0x1) stick_y = 30000;
      if ((bits >> 19) & 0x1) stick_y = -30000;
      if ((bits >> 20) & 0x1) stick = 30000;
      if ((bits >> 21) & 0x1) stick = -30000;
      out_state->gamepad.thumb_lx = stick;
      out_state->gamepad.thumb_ly = stick_y;
      out_state->gamepad.left_trigger = ((bits >> 16) & 0x1) ? 0xFF : 0;
      out_state->gamepad.right_trigger = ((bits >> 17) & 0x1) ? 0xFF : 0;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(in::DeviceId id, uint32_t flags,
                                 in::X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (id != kMandoGuion) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_caps) {
      // Like the SDK's empty pad (nop_input_driver.cpp:47-69).
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      // The sticks too, like the empty pad: with 0 the game did not steer with the autopilot's stick.
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(in::DeviceId id, in::X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    return id == kMandoGuion ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(in::DeviceId id, uint32_t flags,
                              in::X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    if (id != kMandoGuion) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    uint32_t paquete = 0;
    int16_t stick = 0;
    Actualizar(paquete, stick);
    std::lock_guard<std::mutex> cerrojo(mutex_);
    if (teclas_.empty()) {
      return X_ERROR_EMPTY;
    }
    if (out_keystroke) {
      *out_keystroke = teclas_.front();
    }
    teclas_.pop_front();
    return X_ERROR_SUCCESS;
  }

 private:
  // Script buttons at this instant and the autopilot's stick. Each change bumps packet_number and queues
  // the pressed and released keys, for the menus that read keys.
  uint32_t Actualizar(uint32_t& paquete, int16_t& stick) {
    std::lock_guard<std::mutex> cerrojo(mutex_);
    const double t = std::chrono::duration<double>(Reloj::now() - inicio_).count();
    uint32_t bits = g_bits_tras_soltar.load(std::memory_order_relaxed);
    stick = g_stick_lx.load(std::memory_order_relaxed);
    for (const Paso& p : pasos_) {
      if (t >= p.inicio && t < p.fin) {
        bits |= p.bits;
      }
    }
    if (bits != actuales_ || stick != stick_) {
      for (const Boton& b : kBotones) {
        const bool antes = (actuales_ & b.bits) != 0;
        const bool ahora = (bits & b.bits) != 0;
        if (antes == ahora) {
          continue;
        }
        in::X_INPUT_KEYSTROKE tecla{};
        tecla.virtual_key = b.tecla;
        tecla.flags = uint16_t(ahora ? in::X_INPUT_KEYSTROKE_KEYDOWN : in::X_INPUT_KEYSTROKE_KEYUP);
        if (teclas_.size() < 64) {
          teclas_.push_back(tecla);
        }
        REXLOG_INFO("[prueba] mando del guion: {} {} a los {:.1f} s", b.nombre,
                    ahora ? "pulsado" : "suelto", t);
      }
      actuales_ = bits;
      stick_ = stick;
      ++paquete_;
    }
    paquete = paquete_;
    return bits;
  }

  const std::vector<Paso> pasos_;
  const Reloj::time_point inicio_;
  std::mutex mutex_;
  uint32_t actuales_ = 0;
  int16_t stick_ = 0;
  uint32_t paquete_ = 1;
  std::deque<in::X_INPUT_KEYSTROKE> teclas_;
};

uint32_t Leer32(const uint8_t* base, uint32_t direccion) {
  uint32_t v = 0;
  std::memcpy(&v, base + direccion, sizeof(v));
  return __builtin_bswap32(v);
}

constexpr uint32_t kEstadoJuego = 0x82A39AD8;
constexpr uint32_t kEstadoCarrera = 6;
// Only from the game's main thread.
bool g_en_carrera = false;
Reloj::time_point g_inicio_carrera;
bool g_ia_activada = false;
bool g_ia_soltada = false;
Reloj::time_point g_soltada;
std::vector<Paso> g_pasos_tras_soltar;
bool g_pasos_tras_soltar_leidos = false;

// Calls a game function for player 1 in the middle of another one: saves and restores the volatile
// registers. Returns the r3 the function returns with.
uint32_t ComandoJugador1(PPCContext& ctx, uint8_t* base, void (*funcion)(PPCContext&, uint8_t*)) {
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64;
  const uint64_t r7 = ctx.r7.u64, r8 = ctx.r8.u64, r9 = ctx.r9.u64, r10 = ctx.r10.u64;
  const uint64_t lr = ctx.lr;
  ctx.r3.u64 = 0;  // jugador 1
  funcion(ctx, base);
  const uint32_t resultado = ctx.r3.u32;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  ctx.r8.u64 = r8;
  ctx.r9.u64 = r9;
  ctx.r10.u64 = r10;
  ctx.lr = lr;
  return resultado;
}

// After releasing the AI: the nfsmw_prueba_tras_soltar buttons due now.
void BotonesTrasSoltar() {
  if (!g_pasos_tras_soltar_leidos) {
    g_pasos_tras_soltar_leidos = true;
    g_pasos_tras_soltar = Leer(REXCVAR_GET(nfsmw_prueba_tras_soltar));
  }
  const double t = std::chrono::duration<double>(Reloj::now() - g_soltada).count();
  uint32_t bits = 0;
  for (const Paso& p : g_pasos_tras_soltar) {
    if (t >= p.inicio && t < p.fin) {
      bits |= p.bits;
    }
  }
  g_bits_tras_soltar.store(bits, std::memory_order_relaxed);
}

// Dumps for nfsmw_prueba_volcar_coche. Only from the game's main thread.
std::vector<double> g_tiempos_volcado;
bool g_tiempos_volcado_leidos = false;
size_t g_siguiente_volcado = 0;
int g_volcados = 0;

// Guest memory with pages readable on the host, to follow pointers without going out of bounds. Excludes
// the high physical range (from 0xE0000000 it carries the REX_PHYS_HOST_OFFSET offset).
bool Legible(uint8_t* base, uint32_t direccion, uint32_t tamano) {
  if (tamano == 0 || direccion < 0x40000000u || uint64_t(direccion) + tamano > 0xE0000000ull) {
    return false;
  }
  const uint64_t pagina = rex::memory::page_size();
  uint64_t actual = direccion;
  const uint64_t fin = uint64_t(direccion) + tamano;
  while (actual < fin) {
    size_t largo = size_t(fin - actual);
    rex::memory::PageAccess acceso = rex::memory::PageAccess::kNoAccess;
    if (!rex::memory::QueryProtect(base + actual, largo, acceso) ||
        (uint32_t(acceso) & uint32_t(rex::memory::PageAccess::kReadOnly)) == 0) {
      return false;
    }
    // On Windows the length counts from the start of the page.
    const uint64_t siguiente = (actual & ~(pagina - 1)) + largo;
    if (siguiente <= actual) {
      return false;
    }
    actual = siguiente;
  }
  return true;
}

// Dumps to logs/coche_N.bin the memory of player 1's car interfaces and of what they point to.
// sub_82366FB0(0) returns the player's car interface; at +4 is its COM object, with the sorted list of
// {identifier, interface} pairs between +4 and +8 (walked by sub_8231AF38). Format: "NFSCOCHE", u32
// version (1), f64 race seconds and blocks {u32 address, u32 size, u32 source (where the pointer was;
// 0 = direct), guest bytes}; the headers in little endian.
void VolcarCoche(PPCContext& ctx, uint8_t* base, double segundos) {
  const uint32_t interfaz = ComandoJugador1(ctx, base, __imp__sub_82366FB0);
  const uint32_t objeto = Legible(base, interfaz, 8) ? Leer32(base, interfaz + 4) : 0;
  if (!Legible(base, objeto, 16)) {
    REXLOG_WARN("[prueba] coche del jugador a los {:.2f} s: sin objeto (interfaz {:08X}, objeto {:08X})", segundos,
                interfaz, objeto);
    return;
  }
  const uint32_t lista = Leer32(base, objeto + 4);
  const uint32_t lista_fin = Leer32(base, objeto + 8);
  uint32_t interfaces = 0;
  if (lista_fin > lista && (lista_fin - lista) % 8 == 0 && Legible(base, lista, lista_fin - lista)) {
    interfaces = std::min<uint32_t>((lista_fin - lista) / 8, 64);
  }

  struct Bloque {
    uint32_t direccion;
    uint32_t tamano;
    uint32_t origen;
    bool explorar;
  };
  std::vector<Bloque> bloques;
  std::set<uint32_t> vistas;
  const auto anadir = [&](uint32_t direccion, uint32_t tamano, uint32_t origen, bool explorar) {
    if (vistas.insert(direccion).second && Legible(base, direccion, tamano)) {
      bloques.push_back({direccion, tamano, origen, explorar});
    }
  };
  anadir(interfaz - 0x100, 0x1000, 0, true);
  anadir(objeto, 0x40, 0, false);
  anadir(lista, interfaces * 8, 0, false);
  std::string resumen;
  for (uint32_t i = 0; i < interfaces; ++i) {
    const uint32_t identificador = Leer32(base, lista + i * 8);
    const uint32_t puntero = Leer32(base, lista + i * 8 + 4);
    const uint32_t vtabla = Legible(base, puntero, 4) ? Leer32(base, puntero) : 0;
    char texto[32];
    std::snprintf(texto, sizeof(texto), " %08X=%08X/%08X", identificador, puntero, vtabla);
    resumen += texto;
    anadir(puntero - 0x100, 0x1000, 0, true);
  }
  // One level of pointers from the first 0x500 bytes of each interface, excluding the executable image
  // (vtables and globals).
  const size_t directos = bloques.size();
  for (size_t b = 0; b < directos; ++b) {
    if (!bloques[b].explorar) {
      continue;
    }
    for (uint32_t desplazamiento = 0x100; desplazamiento + 4 <= 0x600 && bloques.size() < 3000;
         desplazamiento += 4) {
      const uint32_t valor = Leer32(base, bloques[b].direccion + desplazamiento);
      if ((valor & 3) != 0 || (valor >= 0x80000000u && valor < 0x90000000u)) {
        continue;
      }
      anadir(valor, 0x400, bloques[b].direccion + desplazamiento, false);
    }
  }

  char ruta[48];
  std::snprintf(ruta, sizeof(ruta), "logs/coche_%d.bin", g_volcados++);
  std::FILE* fichero = std::fopen(ruta, "wb");
  if (!fichero) {
    REXLOG_WARN("[prueba] coche del jugador: no se puede crear {}", std::string_view(ruta));
    return;
  }
  const uint32_t version = 1;
  std::fwrite("NFSCOCHE", 1, 8, fichero);
  std::fwrite(&version, sizeof(version), 1, fichero);
  std::fwrite(&segundos, sizeof(segundos), 1, fichero);
  uint64_t bytes = 0;
  for (const Bloque& b : bloques) {
    std::fwrite(&b.direccion, sizeof(b.direccion), 1, fichero);
    std::fwrite(&b.tamano, sizeof(b.tamano), 1, fichero);
    std::fwrite(&b.origen, sizeof(b.origen), 1, fichero);
    std::fwrite(base + b.direccion, 1, b.tamano, fichero);
    bytes += b.tamano;
  }
  std::fclose(fichero);
  REXLOG_INFO("[prueba] coche del jugador a los {:.2f} s: interfaz {:08X}, objeto {:08X}, {} interfaces:{}; {} "
              "bloques ({} bytes) en {}",
              segundos, interfaz, objeto, interfaces, resumen, bloques.size(), bytes, std::string_view(ruta));
}

// nfsmw_prueba_volcar_coche: one dump per time in the list, in order.
void VolcadosCoche(PPCContext& ctx, uint8_t* base, double segundos) {
  if (!g_tiempos_volcado_leidos) {
    g_tiempos_volcado_leidos = true;
    const std::string texto = REXCVAR_GET(nfsmw_prueba_volcar_coche);
    size_t i = 0;
    while (i < texto.size()) {
      size_t j = texto.find(',', i);
      if (j == std::string::npos) {
        j = texto.size();
      }
      double valor = 0.0;
      if (Numero(std::string_view(texto).substr(i, j - i), valor)) {
        g_tiempos_volcado.push_back(valor);
      }
      i = j + 1;
    }
    std::sort(g_tiempos_volcado.begin(), g_tiempos_volcado.end());
  }
  if (g_siguiente_volcado < g_tiempos_volcado.size() && segundos >= g_tiempos_volcado[g_siguiente_volcado]) {
    ++g_siguiente_volcado;
    VolcarCoche(ctx, base, segundos);
  }
}

// Player car state in world coordinates (Z up), from the car's main interface.
struct EstadoCoche {
  double x, y, z;
  double vx, vy, vz;
  double fx, fy;  // "forward" row of the world matrix
};
// Only from the game's main thread; looked up again in every race.
uint32_t g_interfaz_coche = 0;

double LeerF32(const uint8_t* base, uint32_t direccion) {
  const uint32_t bits = Leer32(base, direccion);
  float valor = 0.0f;
  std::memcpy(&valor, &bits, sizeof(valor));
  return valor;
}

bool LeerCoche(PPCContext& ctx, uint8_t* base, EstadoCoche& e) {
  if (g_interfaz_coche == 0) {
    const uint32_t interfaz = ComandoJugador1(ctx, base, __imp__sub_82366FB0);
    if (!Legible(base, interfaz, 0x900)) {
      return false;
    }
    g_interfaz_coche = interfaz;
    REXLOG_INFO("[prueba] coche del jugador: interfaz {:08X}", interfaz);
  }
  const uint32_t i = g_interfaz_coche;
  e.fx = LeerF32(base, i + 0x854);
  e.fy = LeerF32(base, i + 0x858);
  e.x = LeerF32(base, i + 0x884);
  e.y = LeerF32(base, i + 0x888);
  e.z = LeerF32(base, i + 0x88C);
  e.vx = LeerF32(base, i + 0x894);
  e.vy = LeerF32(base, i + 0x898);
  e.vz = LeerF32(base, i + 0x89C);
  return std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z) && std::isfinite(e.vx) &&
         std::isfinite(e.vy) && std::isfinite(e.fx) && std::isfinite(e.fy);
}

// logs/traza_coche.csv (nfsmw_prueba_traza_coche). Flushed every half second: the tests end by killing the
// process. modo: 0 = no AI, 1 = AI, 2 = AI released by time, 3 = autopilot.
std::FILE* g_traza = nullptr;
bool g_traza_intentada = false;
int g_traza_lineas = 0;

void Trazar(double segundos, const EstadoCoche& e, int modo, int16_t stick) {
  if (!g_traza_intentada) {
    g_traza_intentada = true;
    g_traza = std::fopen("logs/traza_coche.csv", "w");
    if (!g_traza) {
      REXLOG_WARN("[prueba] traza del coche: no se puede crear logs/traza_coche.csv");
      return;
    }
    std::fprintf(g_traza, "reloj_ms,segundos,x,y,z,vx,vy,vz,fx,fy,modo,stick\n");
  }
  if (!g_traza) {
    return;
  }
  const long long reloj = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
  std::fprintf(g_traza, "%lld,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%d,%d\n", reloj, segundos, e.x, e.y, e.z,
               e.vx, e.vy, e.vz, e.fx, e.fy, modo, int(stick));
  if (++g_traza_lineas % 30 == 0) {
    std::fflush(g_traza);
  }
}

// Autopilot route (nfsmw_prueba_piloto): signed world x,y points; the first is where the AI is released.
// g_acumulado: meters from the first point to each point. g_tramo: the segment from g_tramo - 1 to g_tramo.
enum class Piloto { kEsperando, kConduciendo, kTerminado };
Piloto g_piloto = Piloto::kEsperando;
bool g_linea_leida = false;
bool g_hay_linea = false;
std::vector<double> g_px, g_py, g_acumulado;
size_t g_tramo = 1;
Reloj::time_point g_piloto_inicio;
double g_piloto_informe = 0.0;
constexpr uint32_t kBitRt = 1u << 17;
constexpr double kPi = 3.14159265358979323846;

bool LeerLinea() {
  if (g_linea_leida) {
    return g_hay_linea;
  }
  g_linea_leida = true;
  const std::string texto = REXCVAR_GET(nfsmw_prueba_piloto);
  if (texto.empty()) {
    return false;
  }
  std::vector<double> numeros;
  size_t i = 0;
  while (i <= texto.size()) {
    size_t j = texto.find(',', i);
    if (j == std::string::npos) {
      j = texto.size();
    }
    const std::string trozo = texto.substr(i, j - i);
    char* fin = nullptr;
    const double valor = std::strtod(trozo.c_str(), &fin);
    if (trozo.empty() || fin != trozo.c_str() + trozo.size() || !std::isfinite(valor)) {
      numeros.clear();
      break;
    }
    numeros.push_back(valor);
    i = j + 1;
  }
  if (numeros.size() < 4 || numeros.size() % 2 != 0) {
    REXLOG_WARN("[prueba] piloto: ruta no valida \"{}\" (hacen falta al menos 2 puntos x,y)", texto);
    return false;
  }
  for (size_t k = 0; k < numeros.size(); k += 2) {
    g_px.push_back(numeros[k]);
    g_py.push_back(numeros[k + 1]);
  }
  g_acumulado.assign(g_px.size(), 0.0);
  for (size_t k = 1; k < g_px.size(); ++k) {
    const double largo = std::hypot(g_px[k] - g_px[k - 1], g_py[k] - g_py[k - 1]);
    if (largo < 1.0) {
      REXLOG_WARN("[prueba] piloto: ruta no valida \"{}\" (los puntos {} y {} estan a menos de 1 m)", texto, k,
                  k + 1);
      g_px.clear();
      g_py.clear();
      g_acumulado.clear();
      return false;
    }
    g_acumulado[k] = g_acumulado[k - 1] + largo;
  }
  g_hay_linea = true;
  REXLOG_INFO("[prueba] piloto: ruta de {} puntos desde ({:.1f}, {:.1f}), {:.1f} m", g_px.size(), g_px[0], g_py[0],
              g_acumulado.back());
  return true;
}

// Route point 'metros' from the first point; before the first and after the last it extends the end
// segment.
void PuntoDeRuta(double metros, double& x, double& y) {
  size_t k = 1;
  while (k + 1 < g_px.size() && g_acumulado[k] < metros) {
    ++k;
  }
  const double f = (metros - g_acumulado[k - 1]) / (g_acumulado[k] - g_acumulado[k - 1]);
  x = g_px[k - 1] + (g_px[k] - g_px[k - 1]) * f;
  y = g_py[k - 1] + (g_py[k] - g_py[k - 1]) * f;
}

// Shortcut autopilot: returns true while it drives the car. Pure pursuit: it aims at a route point
// nfsmw_prueba_piloto_adelanto meters ahead of the car's projection onto its segment.
bool Pilotar(PPCContext& ctx, uint8_t* base, double segundos, const EstadoCoche& e) {
  const double distancia_a = std::hypot(e.x - g_px[0], e.y - g_py[0]);
  const double velocidad = std::hypot(e.vx, e.vy);
  if (g_piloto == Piloto::kTerminado) {
    if (distancia_a > 300.0) {
      g_piloto = Piloto::kEsperando;
    }
    return false;
  }
  if (g_piloto == Piloto::kEsperando) {
    if (!g_ia_activada || g_ia_soltada || distancia_a > REXCVAR_GET(nfsmw_prueba_piloto_radio)) {
      return false;
    }
    ComandoJugador1(ctx, base, __imp__sub_823671C0);
    g_piloto = Piloto::kConduciendo;
    g_piloto_inicio = Reloj::now();
    g_piloto_informe = 0.0;
    g_tramo = 1;
    REXLOG_INFO("[prueba] piloto: suelta la IA a {:.1f} m del primer punto, en ({:.1f}, {:.1f}), {:.1f} m/s, a los "
                "{:.1f} s de carrera",
                distancia_a, e.x, e.y, velocidad, segundos);
  }
  // Projection onto the current segment; moves to the next one when the projection passes the segment's end.
  double recorrido = 0.0;
  double lateral = 0.0;  // > 0: left of the segment
  for (;;) {
    const size_t k = g_tramo;
    const double largo = g_acumulado[k] - g_acumulado[k - 1];
    const double ux = (g_px[k] - g_px[k - 1]) / largo;
    const double uy = (g_py[k] - g_py[k - 1]) / largo;
    const double rx = e.x - g_px[k - 1];
    const double ry = e.y - g_py[k - 1];
    const double a_lo_largo = rx * ux + ry * uy;
    if (a_lo_largo > largo && k + 1 < g_px.size()) {
      ++g_tramo;
      continue;
    }
    recorrido = g_acumulado[k - 1] + a_lo_largo;
    lateral = -rx * uy + ry * ux;
    break;
  }
  const double total = g_acumulado.back();
  const double t = std::chrono::duration<double>(Reloj::now() - g_piloto_inicio).count();
  if (recorrido >= total || t >= REXCVAR_GET(nfsmw_prueba_piloto_max_s)) {
    g_stick_lx.store(0, std::memory_order_relaxed);
    g_bits_tras_soltar.store(0, std::memory_order_relaxed);
    ComandoJugador1(ctx, base, __imp__sub_82367128);
    g_piloto = Piloto::kTerminado;
    REXLOG_INFO("[prueba] piloto: la IA vuelve a conducir en ({:.1f}, {:.1f}) tras {:.1f} s, tramo {}, recorrido "
                "{:.1f} de {:.1f} m, lateral {:+.1f} m, {:.1f} m/s",
                e.x, e.y, t, g_tramo, recorrido, total, lateral, velocidad);
    return true;
  }
  double tx = 0.0;
  double ty = 0.0;
  PuntoDeRuta(recorrido + REXCVAR_GET(nfsmw_prueba_piloto_adelanto), tx, ty);
  tx -= e.x;
  ty -= e.y;
  const double hx = velocidad > 5.0 ? e.vx : e.fx;
  const double hy = velocidad > 5.0 ? e.vy : e.fy;
  const double error = std::atan2(hx * ty - hy * tx, hx * tx + hy * ty);  // > 0: the target is to the left
  const double completo = REXCVAR_GET(nfsmw_prueba_piloto_angulo) * kPi / 180.0;
  double mando = std::clamp(-error / completo, -1.0, 1.0) * REXCVAR_GET(nfsmw_prueba_piloto_signo);
  // Below the dead zone the game does not steer: corrections start at its edge, except errors under 2 % of
  // full lock, which stay centered to avoid swerving.
  const double zona = std::clamp(REXCVAR_GET(nfsmw_prueba_piloto_zona_muerta), 0.0, 0.9);
  mando = std::abs(mando) < 0.02 ? 0.0 : std::copysign(zona + (1.0 - zona) * std::abs(mando), mando);
  const int16_t stick = int16_t(std::lround(std::clamp(mando, -1.0, 1.0) * 32767.0));
  g_stick_lx.store(stick, std::memory_order_relaxed);
  g_bits_tras_soltar.store(kBitRt, std::memory_order_relaxed);
  if (t >= g_piloto_informe) {
    g_piloto_informe += 0.5;
    REXLOG_INFO("[prueba] piloto: {:.1f} s, ({:.1f}, {:.1f}), tramo {}, recorrido {:.1f} m, lateral {:+.1f} m, "
                "error {:+.1f} grados, stick {}, {:.1f} m/s",
                t, e.x, e.y, g_tramo, recorrido, lateral, error * 180.0 / kPi, stick, velocidad);
  }
  return true;
}

}  // namespace

// Once per race, nfsmw_prueba_ia_conduce_retraso_s after entering the race state (after the intro and the
// countdown), the game's ForceAIControl command for player 1. It rearms when leaving the race.
void IaConduce(PPCContext& ctx, uint8_t* base) {
  if (Leer32(base, kEstadoJuego) != kEstadoCarrera) {
    g_en_carrera = false;
    g_ia_activada = false;
    g_ia_soltada = false;
    g_bits_tras_soltar.store(0, std::memory_order_relaxed);
    g_siguiente_volcado = 0;
    g_interfaz_coche = 0;
    if (g_piloto == Piloto::kConduciendo) {
      g_stick_lx.store(0, std::memory_order_relaxed);
    }
    g_piloto = Piloto::kEsperando;
    if (g_traza) {
      std::fflush(g_traza);
    }
    return;
  }
  if (!g_en_carrera) {
    g_en_carrera = true;
    g_inicio_carrera = Reloj::now();
    return;
  }
  const double segundos = std::chrono::duration<double>(Reloj::now() - g_inicio_carrera).count();
  VolcadosCoche(ctx, base, segundos);
  const bool linea = LeerLinea();
  const bool traza = REXCVAR_GET(nfsmw_prueba_traza_coche);
  if (linea || traza) {
    EstadoCoche coche{};
    if (LeerCoche(ctx, base, coche)) {
      const bool pilotando = linea && Pilotar(ctx, base, segundos, coche);
      if (traza) {
        const int modo = g_piloto == Piloto::kConduciendo ? 3 : g_ia_soltada ? 2 : g_ia_activada ? 1 : 0;
        Trazar(segundos, coche, modo, g_stick_lx.load(std::memory_order_relaxed));
      }
      if (pilotando) {
        return;
      }
    }
  }
  if (g_ia_soltada) {
    BotonesTrasSoltar();
    return;
  }
  if (g_ia_activada) {
    // Alley shortcut: after nfsmw_prueba_ia_suelta_s seconds of racing the car is taken from the AI.
    const double suelta_s = REXCVAR_GET(nfsmw_prueba_ia_suelta_s);
    if (suelta_s > 0.0 && segundos >= suelta_s) {
      ComandoJugador1(ctx, base, __imp__sub_823671C0);
      g_ia_soltada = true;
      g_soltada = Reloj::now();
      REXLOG_INFO("[prueba] carrera: la IA suelta el coche del jugador ({:.1f} s despues de entrar en la carrera); "
                  "botones desde ahora: \"{}\"",
                  segundos, REXCVAR_GET(nfsmw_prueba_tras_soltar));
      BotonesTrasSoltar();
    }
    return;
  }
  if (segundos < REXCVAR_GET(nfsmw_prueba_ia_conduce_retraso_s)) {
    return;
  }
  ComandoJugador1(ctx, base, __imp__sub_82367128);
  g_ia_activada = true;
  REXLOG_INFO("[prueba] carrera: la IA del juego conduce el coche del jugador (ForceAIControl, {:.1f} s "
              "despues de entrar en la carrera)",
              segundos);
}

void EnvolverEntrada(rex::RuntimeConfig& config) {
  const std::string guion = REXCVAR_GET(nfsmw_prueba_botones);
  if (guion.empty() || !config.input_factory) {
    return;
  }
  std::vector<Paso> pasos = Leer(guion);
  if (pasos.empty()) {
    REXLOG_WARN("[prueba] mando del guion: \"{}\" no tiene pasos validos", guion);
    return;
  }
  auto base = config.input_factory;
  config.input_factory =
      [base, pasos](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto sistema = base(tool_mode);
    if (sistema && !tool_mode) {
      // The default factory is CreateDefaultInputSystem (ui/rex_app.cpp:337), which
      // returns a rex::input::InputSystem.
      static_cast<in::InputSystem*>(sistema.get())->AddDriver(std::make_unique<MandoGuion>(pasos));
      REXLOG_INFO("[prueba] mando del guion con {} pasos", pasos.size());
    }
    return sistema;
  };
}

}  // namespace nfsmw::prueba

// Render of each game frame (main thread): used as the clock to enable the AI in a race.
REX_HOOK_RAW(sub_824411B8) {
  if (REXCVAR_GET(nfsmw_prueba_ia_conduce)) {
    nfsmw::prueba::IaConduce(ctx, base);
  }
  __imp__sub_824411B8(ctx, base);
}
