// deadspace2 - the port's options menu: ZL + ZR + Minus opens it over the game (the presenter paints it,
// sdk-patches/0012: no ImGui, which would move presentation to the UI thread), and while it is open the game sees the
// controller idle. Up / Down choose the row, Left / Right change it, A saves, B closes. Ported from ds3-nx
// (src/ds3_menu.cpp). Two rows:
//   - the scene resolution (ds2_resolution.h): 720p for overclocked consoles, 540p for stock clocks; the game sets its
//     scene size at start, so a new choice applies at the next start;
//   - the vibration strength (input_vibracion_fuerza, sdk-patches/0015): the game only has vibration on / off, and its
//     values are for the Xbox 360 pad's motors, much gentler than the Switch's HD Rumble at full amplitude. Applied at
//     once and kept in ds2_vibracion.txt (percent); without it, 50 %.

#include "ds2_menu.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/filtro_mando.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/system.h>
#include <rex/ui/superposicion.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#include "ds2_resolution.h"

REXCVAR_DECLARE(double, input_vibracion_fuerza);  // the SDK's (src/input/switch/switch_input_driver.cpp)

namespace ds2::menu {
namespace {

#if defined(__SWITCH__)
constexpr uint64_t kCombinacion = HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Minus;
constexpr uint64_t kArriba = HidNpadButton_Up | HidNpadButton_StickLUp;
constexpr uint64_t kAbajo = HidNpadButton_Down | HidNpadButton_StickLDown;
constexpr uint64_t kIzquierda = HidNpadButton_Left | HidNpadButton_StickLLeft;
constexpr uint64_t kDerecha = HidNpadButton_Right | HidNpadButton_StickLRight;
constexpr uint64_t kAceptar = HidNpadButton_A;
constexpr uint64_t kCerrar = HidNpadButton_B;
#else
constexpr uint64_t kCombinacion = 0, kArriba = 0, kAbajo = 0, kIzquierda = 0, kDerecha = 0, kAceptar = 0,
                   kCerrar = 0;
#endif

// The vibration strengths offered, in percent of the game's.
constexpr std::array<uint32_t, 5> kFuerzas = {0, 25, 50, 75, 100};

struct Estado {
  std::mutex candado;
  bool abierto = false;
  uint32_t fila = 0;        // 0: resolution, 1: vibration
  uint32_t resolucion = 0;  // 0: 720p, 1: 540p
  uint32_t fuerza = 2;      // index into kFuerzas
  std::string aviso;        // the last line: what happened
  std::array<uint64_t, 8> antes{};  // buttons held at each slot's previous poll
};
Estado& ElEstado() {
  static Estado e;
  return e;
}

const uint8_t* g_memoria = nullptr;  // the guest's address space (the scene size the game is using)

std::filesystem::path FicheroVibracion() {
  return rex::filesystem::GetExecutableFolder() / "ds2_vibracion.txt";
}

// The saved vibration strength in percent, or -1 if there is none.
int32_t VibracionGuardada() {
  FILE* f = std::fopen(FicheroVibracion().string().c_str(), "r");
  if (!f) return -1;
  unsigned valor = 0;
  const bool leido = std::fscanf(f, "%u", &valor) == 1;
  std::fclose(f);
  return leido && valor <= 100 ? int32_t(valor) : -1;
}

bool GuardarVibracion(uint32_t porcentaje) {
  FILE* f = std::fopen(FicheroVibracion().string().c_str(), "w");
  if (!f) return false;
  const bool escrito = std::fprintf(f, "%u\n", unsigned(porcentaje)) > 0;
  return std::fclose(f) == 0 && escrito;
}

uint32_t IndiceFuerza(uint32_t porcentaje) {
  uint32_t mejor = 0;
  for (uint32_t i = 1; i < kFuerzas.size(); ++i) {
    const int32_t d = int32_t(kFuerzas[i]) - int32_t(porcentaje), dm = int32_t(kFuerzas[mejor]) - int32_t(porcentaje);
    if ((d < 0 ? -d : d) < (dm < 0 ? -dm : dm)) mejor = i;
  }
  return mejor;
}

void AplicarFuerza(uint32_t porcentaje) { REXCVAR_SET(input_vibracion_fuerza, double(porcentaje) / 100.0); }

uint32_t ResolucionDeOpcion(uint32_t opcion) { return opcion == 0 ? 720 : 540; }

void Pintar(const Estado& e) {
  std::string ahora = "?";
  if (g_memoria) {
    char texto[32];
    std::snprintf(texto, sizeof(texto), "%ux%u",
                  unsigned(rex::memory::load_and_swap<uint16_t>(g_memoria + resolucion::kAnchoEscena)),
                  unsigned(rex::memory::load_and_swap<uint16_t>(g_memoria + resolucion::kAltoEscena)));
    ahora = texto;
  }
  const uint32_t guardada = resolucion::Guardada();
  const char* res = e.resolucion == 0 ? "720P  1280x720  (OVERCLOCK)" : "540P   960x540  (STOCK CLOCKS)";
  std::vector<std::string> lineas = {
      "DEAD SPACE 2 - OPTIONS",
      std::string(e.fila == 0 ? "> " : "  ") + "RESOLUTION:  < " + res + " >",
      "    NOW " + ahora + ", SAVED " + (guardada ? std::to_string(guardada) + "P" : std::string("AUTO")) +
          " (APPLIES AT THE NEXT START)",
      std::string(e.fila == 1 ? "> " : "  ") + "VIBRATION:   < " + std::to_string(kFuerzas[e.fuerza]) + "% >",
      "UP/DOWN: ROW   LEFT/RIGHT: CHANGE   A: SAVE   B: CLOSE",
      e.aviso,
  };
  rex::ui::FijarSuperposicion(std::move(lineas));
}

bool Filtro(size_t ranura, uint64_t botones) {
  Estado& e = ElEstado();
  std::lock_guard<std::mutex> cerrojo(e.candado);
  const size_t i = ranura < e.antes.size() ? ranura : e.antes.size() - 1;
  const uint64_t nuevos = botones & ~e.antes[i];
  e.antes[i] = botones;
  if (!e.abierto) {
    if ((botones & kCombinacion) != kCombinacion || !(nuevos & kCombinacion)) return false;
    e.abierto = true;
    e.fila = 0;
    e.resolucion = resolucion::Elegida() == 540 ? 1 : 0;
    e.fuerza = IndiceFuerza(uint32_t(REXCVAR_GET(input_vibracion_fuerza) * 100.0 + 0.5));
    e.aviso.clear();
    REXLOG_INFO("ds2: menu de opciones abierto");
    Pintar(e);
    return true;
  }
  if (nuevos & kCerrar) {
    e.abierto = false;
    rex::ui::FijarSuperposicion({});
    REXLOG_INFO("ds2: menu de opciones cerrado");
    return true;
  }
  if (nuevos & (kArriba | kAbajo)) {
    e.fila = e.fila ? 0 : 1;
    e.aviso.clear();
    Pintar(e);
  } else if (nuevos & (kIzquierda | kDerecha)) {
    const bool derecha = (nuevos & kDerecha) != 0;
    if (e.fila == 0) {
      e.resolucion = e.resolucion ? 0 : 1;
    } else if (derecha && e.fuerza + 1 < kFuerzas.size()) {
      ++e.fuerza;
    } else if (!derecha && e.fuerza > 0) {
      --e.fuerza;
    }
    if (e.fila == 1) AplicarFuerza(kFuerzas[e.fuerza]);  // felt at once, saved with A
    e.aviso.clear();
    Pintar(e);
  } else if (nuevos & kAceptar) {
    const uint32_t valor = ResolucionDeOpcion(e.resolucion);
    const bool res_ok = resolucion::Guardar(valor);
    const bool vib_ok = GuardarVibracion(kFuerzas[e.fuerza]);
    AplicarFuerza(kFuerzas[e.fuerza]);
    e.aviso = res_ok && vib_ok ? "SAVED. RESOLUTION APPLIES AT THE NEXT START" : "COULD NOT SAVE (SD CARD?)";
    REXLOG_INFO("ds2: opciones guardadas: resolucion {}p, vibracion {}% ({})", valor, kFuerzas[e.fuerza],
                res_ok && vib_ok ? "ok" : "FALLO");
    Pintar(e);
  }
  return true;  // the game does not see the controller while the menu is open
}

}  // namespace

void Instalar(rex::Runtime& runtime) {
  if (runtime.memory()) g_memoria = runtime.memory()->TranslateVirtual<const uint8_t*>(0);
  const int32_t vibracion = VibracionGuardada();
  if (vibracion >= 0) AplicarFuerza(uint32_t(vibracion));  // else ds2.toml's, or 50 % (ds2_app.h)
  rex::input::FijarFiltroMando(&Filtro);
  REXLOG_INFO("ds2: menu de opciones con ZL + ZR + Menos (resolucion guardada: {}, por defecto {}p; vibracion {}%{})",
              resolucion::Guardada(), resolucion::PorDefecto(),
              uint32_t(REXCVAR_GET(input_vibracion_fuerza) * 100.0 + 0.5), vibracion >= 0 ? ", guardada" : "");
}

}  // namespace ds2::menu
