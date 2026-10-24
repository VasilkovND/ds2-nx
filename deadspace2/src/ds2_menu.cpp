// deadspace2 - the port's options menu: ZL + ZR + Minus opens it over the game (the presenter paints it,
// sdk-patches/0012: no ImGui, which would move presentation to the UI thread), and while it is open the game sees the
// controller idle. Up / Down choose, A saves, B closes. For now it holds the scene resolution (ds2_resolution.h): 720p
// for overclocked consoles, 540p for stock clocks; the game sets its scene size at start, so a new choice applies at
// the next start. Ported from ds3-nx (src/ds3_menu.cpp).

#include "ds2_menu.h"

#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

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

namespace ds2::menu {
namespace {

#if defined(__SWITCH__)
constexpr uint64_t kCombinacion = HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Minus;
constexpr uint64_t kArriba = HidNpadButton_Up | HidNpadButton_StickLUp;
constexpr uint64_t kAbajo = HidNpadButton_Down | HidNpadButton_StickLDown;
constexpr uint64_t kAceptar = HidNpadButton_A;
constexpr uint64_t kCerrar = HidNpadButton_B;
#else
constexpr uint64_t kCombinacion = 0, kArriba = 0, kAbajo = 0, kAceptar = 0, kCerrar = 0;
#endif

struct Estado {
  std::mutex candado;
  bool abierto = false;
  uint32_t opcion = 0;  // 0: 720p, 1: 540p
  std::string aviso;    // the last line: what happened
  std::array<uint64_t, 8> antes{};  // buttons held at each slot's previous poll
};
Estado& ElEstado() {
  static Estado e;
  return e;
}

const uint8_t* g_memoria = nullptr;  // the guest's address space (the scene size the game is using)

uint32_t ValorDeOpcion(uint32_t opcion) { return opcion == 0 ? 720 : 540; }

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
  std::vector<std::string> lineas = {
      "DEAD SPACE 2 - OPTIONS",
      "RESOLUTION:",
      std::string(e.opcion == 0 ? "> " : "  ") + "720P  1280x720  (OVERCLOCK)",
      std::string(e.opcion == 1 ? "> " : "  ") + "540P   960x540  (STOCK CLOCKS)",
      "NOW " + ahora + ", SAVED " + (guardada ? std::to_string(guardada) + "P" : std::string("AUTO")),
      "UP/DOWN: CHOOSE   A: SAVE   B: CLOSE",
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
    e.opcion = resolucion::Elegida() == 540 ? 1 : 0;
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
    e.opcion = e.opcion ? 0 : 1;
    e.aviso.clear();
    Pintar(e);
  } else if (nuevos & kAceptar) {
    const uint32_t valor = ValorDeOpcion(e.opcion);
    const bool guardado = resolucion::Guardar(valor);
    e.aviso = guardado ? "SAVED. RESTART THE GAME TO APPLY IT!" : "COULD NOT SAVE (SD CARD?)";
    REXLOG_INFO("ds2: resolucion {}p {}", valor, guardado ? "guardada" : "NO guardada");
    Pintar(e);
  }
  return true;  // the game does not see the controller while the menu is open
}

}  // namespace

void Instalar(rex::Runtime& runtime) {
  if (runtime.memory()) g_memoria = runtime.memory()->TranslateVirtual<const uint8_t*>(0);
  rex::input::FijarFiltroMando(&Filtro);
  REXLOG_INFO("ds2: menu de opciones con ZL + ZR + Menos (resolucion guardada: {}, por defecto {}p)",
              resolucion::Guardada(), resolucion::PorDefecto());
}

}  // namespace ds2::menu
