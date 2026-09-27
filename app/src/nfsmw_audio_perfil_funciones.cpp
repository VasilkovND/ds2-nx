// nfsmw - diagnostic: timing of the sound engine functions on the audio server thread
//
// On the console, the remaining robotic audio shows up in hard crashes, with the game's audio server thread at
// 70-94 % of a core: its mixing costs almost a whole core. To make it cheaper we need to know which sound
// engine functions take that time, on PC and on the console.
//
// With nfsmw_audio_diag_funciones, only on the server thread (marked by its wait, in nfsmw_audio_servidor.cpp),
// each function in the list measures:
//  - its self time: its duration minus that of the other measured functions it calls. These can be added up;
//  - its inclusive time, counted only in the outermost call. The functions of the sound graph call themselves
//    recursively, and an earlier version that added up every level reached 391 % of the thread's CPU;
//  - its calls and its maximum self time in a 500 ms window (to catch the crashes).
// Every 10 s a summary sorted by self time is logged, with the thread's CPU. These are wall-clock times: they
// include the time the thread is blocked inside the function.
// When off, which is the normal case, each hook only checks the cvar and calls the original function.
//
// The large per-call self times did not come from the bodies of those functions but from what they call
// without being measured, often through pointers. That is why those calls are measured and their targets
// are logged:
//  - the voice's virtual call (sub_825EB0E8, method +8 of the object r3 + (byte [r5 + 73] + 16) * 8) goes to
//    sub_825E1CD0, which reads what the XMA decoder leaves;
//  - the effects pointer of sub_825DCED8 (0x82A2B1C8) points to sub_825FDFB0, the gain-scaled sum (native in
//    nfsmw_audio_suma.cpp);
//  - sub_825D24E0 walks the list at 0x82C5E214 (+0 next, +8 function, +12 argument) and calls each function;
//  - sub_825DA1E0 drains the command queue at 0x82A2AD38 (count at +0, 8-byte entries from +8 with type, index
//    and argument) and calls method +84 or +80 of the object in the table at +772.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "nfsmw_audio_nativo.h"

REXCVAR_DEFINE_BOOL(nfsmw_audio_diag_funciones, false, "NFSMW",
                    "Diagnostico: tiempo propio, tiempo inclusivo y llamadas de las funciones del motor de sonido del "
                    "juego en el hilo servidor de audio, con un resumen cada 10 s en el log");

namespace nfsmw::hilos {
// nfsmw_hilos_switch.cpp
uint64_t ManejadorHiloActual();
int64_t CpuHiloUs(uint64_t manejador);
}  // namespace nfsmw::hilos

namespace nfsmw::audio_perfil {

// Order of the kFunciones table.
enum Indice : size_t {
  kOrdenes,
  kBucle825E4160,
  kBucle825D07C8,
  kGrafo825CFCC8,
  kMezclaPaquete,
  k825DD288,
  k825DCED8,
  k825D2538,
  k825ED568,
  k825DBD68,
  k825DCCB0,
  k825DC0C0,
  kVoz,
  kRemuestreo,
  kRemuestreoLineal,
  kRemuestreo826033A0,
  k826022C8,
  k826047B0,
  k8262E220,
  k82619820,
  k82612270,
  k825E1370,
  k825CD088,
  kAyuda826BDD90,
  k82601A08,
  k825D05F8,
  k825DE890,
  k826027F0,
  k825F2F60,
  k825D24E0,
  k825ED268,
  k825DA1E0,
  k825FDFB0,
  k825E1CD0,
  kNumero
};

namespace {

struct Funcion {
  const char* nombre;
  const char* papel;
};

// Call tree seen in the recompiled code and in a PC profiling run: the server loop (sub_825E3E28) calls the
// commands and sub_825E4160 and sub_825D07C8; sub_825ED350 runs once per packet; sub_825EB0E8, once per voice.
constexpr std::array<Funcion, kNumero> kFunciones = {{
    {"sub_825CF780", "ordenes de audio"},
    {"sub_825E4160", "del bucle"},
    {"sub_825D07C8", "del bucle y del grafo"},
    {"sub_825CFCC8", "grafo de sonido"},
    {"sub_825ED350", "una vez por paquete"},
    {"sub_825DD288", "de 825ED350"},
    {"sub_825DCED8", "de 825DD288"},
    {"sub_825D2538", "de 825ED350"},
    {"sub_825ED568", "de 825ED350"},
    {"sub_825DBD68", "de 825DCED8, hoja"},
    {"sub_825DCCB0", "de 825DCED8"},
    {"sub_825DC0C0", "de 825DCED8"},
    {"sub_825EB0E8", "una vez por voz"},
    {"sub_82602BE0", "llama a los remuestreadores"},
    {"sub_826031C0", "remuestreador lineal"},
    {"sub_826033A0", "otro remuestreador"},
    {"sub_826022C8", "de 825EB0E8"},
    {"sub_826047B0", "de 826022C8, 9 veces"},
    {"sub_8262E220", "de 825EB0E8"},
    {"sub_82619820", "hoja"},
    {"sub_82612270", "llamada indirecta"},
    {"sub_825E1370", "cambio de bytes"},
    {"sub_825CD088", "llamada desde 1 sitio"},
    {"sub_826BDD90", "ayuda con 950 llamadores"},
    {"sub_82601A08", "de 825E4160, hoja"},
    {"sub_825D05F8", "de 825D07C8, hoja"},
    {"sub_825DE890", "memoria de trabajo de la voz y del paquete"},
    {"sub_826027F0", "de 825EB0E8"},
    {"sub_825F2F60", "de 825ED350"},
    {"sub_825D24E0", "lista de 0x82C5E214, por tramo"},
    {"sub_825ED268", "de 825ED350, por tramo"},
    {"sub_825DA1E0", "cola de ordenes de 0x82A2AD38, por tramo"},
    {"sub_825FDFB0", "suma con ganancia, de 825DCED8"},
    {"sub_825E1CD0", "lectura de la voz (XMA)"},
}};

struct Cuenta {
  uint64_t llamadas = 0;
  int64_t propio_ns = 0;
  int64_t inclusivo_ns = 0;
  int64_t propio_ventana_ns = 0;
  int64_t max_propio_ventana_ns = 0;
};

// Targets of a call through a pointer.
struct Destino {
  uint32_t direccion = 0;
  uint64_t veces = 0;
};

struct Destinos {
  std::array<Destino, 8> tabla{};
  uint64_t otros = 0;     // did not fit in the table
  uint64_t sin_leer = 0;  // pointers outside the expected ranges

  void Anotar(uint32_t destino) {
    if (destino < 0x82000000 || destino >= 0x83000000) {
      ++sin_leer;
      return;
    }
    for (Destino& d : tabla) {
      if (d.direccion == destino || d.veces == 0) {
        d.direccion = destino;
        ++d.veces;
        return;
      }
    }
    ++otros;
  }

  std::string Texto() const {
    std::string texto;
    for (const Destino& d : tabla) {
      if (d.veces != 0) {
        texto += fmt::format("{}sub_{:08X} {}", texto.empty() ? "" : ", ", d.direccion, d.veces);
      }
    }
    return fmt::format("{}; otros destinos {}, sin leer {}", texto.empty() ? std::string("ninguna") : texto, otros,
                       sin_leer);
  }
};

class Pila;

// Only the server thread touches all of this.
std::array<Cuenta, kNumero> g_cuentas{};
std::array<int, kNumero> g_activas{};  // calls in progress for each function: detects recursion
int64_t g_medido_ns = 0;               // time of the outermost measured calls, = sum of the self times
int64_t g_desde_ns = 0;
int64_t g_ventana_desde_ns = 0;
int64_t g_cpu_antes_us = -1;
uint64_t g_manejador = 0;
thread_local bool t_servidor = false;
Destinos g_destinos_voz;
Destinos g_destinos_efecto;
Destinos g_destinos_lista;
Destinos g_destinos_cola;

constexpr int64_t kVentanaNs = 500'000'000;
constexpr int64_t kResumenNs = 10'000'000'000;
// sub_825DCED8: lis r9,-32093; addi r31,r9,-20288 (0x82A2B0C0) and lwz r4,264(r31) before its bctrl.
constexpr uint32_t kPunteroEfecto = 0x82A2B0C0 + 264;
// sub_825D24E0: lis r11,-32058; lwz r11,-7660(r11).
constexpr uint32_t kCabezaLista = 0x82C5E214;
// sub_825DA1E0: lis r11,-32093; addi r30,r11,-21192.
constexpr uint32_t kColaOrdenes = 0x82A2AD38;

int64_t AhoraNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void CerrarVentana() {
  for (Cuenta& c : g_cuentas) {
    c.max_propio_ventana_ns = std::max(c.max_propio_ventana_ns, c.propio_ventana_ns);
    c.propio_ventana_ns = 0;
  }
}

// The same reads sub_825EB0E8 does before its bctrl (0x825EB16C-0x825EB190), done on entry. Only pointers
// that land where expected are followed: the object in the heap (above 64 KB) and the virtual table in the
// XEX image.
void AnotarDestinoVoz(uint8_t* base, uint32_t r3, uint32_t r5) {
  using nfsmw::audio_nativo::Dir;
  using nfsmw::audio_nativo::Leer32;
  const uint32_t indice = *Dir(base, r5 + 73);
  const uint32_t fuente = r3 >= 0x10000 ? Leer32(base, r3 + ((indice + 16) << 3)) : 0;
  const uint32_t tabla = fuente >= 0x10000 ? Leer32(base, fuente) : 0;
  const bool tabla_valida = tabla >= 0x82000000 && tabla < 0x83000000;
  g_destinos_voz.Anotar(tabla_valida ? Leer32(base, tabla + 8) : 0);
}

void AnotarDestinoEfecto(uint8_t* base) {
  g_destinos_efecto.Anotar(nfsmw::audio_nativo::Leer32(base, kPunteroEfecto));
}

// The functions of the list that sub_825D24E0 is about to walk (at most 32 nodes).
void AnotarDestinosLista(uint8_t* base) {
  using nfsmw::audio_nativo::Leer32;
  uint32_t nodo = Leer32(base, kCabezaLista);
  for (int i = 0; i < 32 && nodo >= 0x10000; ++i) {
    g_destinos_lista.Anotar(Leer32(base, nodo + 8));
    nodo = Leer32(base, nodo + 0);
  }
}

// The methods sub_825DA1E0 is about to call when draining its queue (at most 64 entries).
void AnotarDestinosCola(uint8_t* base) {
  using nfsmw::audio_nativo::Leer16;
  using nfsmw::audio_nativo::Leer32;
  const int32_t cuenta = int32_t(Leer32(base, kColaOrdenes));
  for (int32_t k = 0; k < std::min(cuenta, 64); ++k) {
    const uint32_t entrada = kColaOrdenes + 8 + uint32_t(k) * 8;
    const uint32_t tipo = Leer16(base, entrada - 4);
    const uint32_t indice = Leer16(base, entrada - 2);
    const uint32_t objeto = Leer32(base, kColaOrdenes + 772 + indice * 4);
    g_destinos_cola.Anotar(objeto >= 0x10000 ? Leer32(base, objeto + (tipo == 0 ? 84 : 80)) : 0);
  }
}

void Informar(int64_t ahora) {
  CerrarVentana();
  const int64_t cpu_us = nfsmw::hilos::CpuHiloUs(g_manejador);
  const int64_t cpu_ms = cpu_us >= 0 && g_cpu_antes_us >= 0 ? (cpu_us - g_cpu_antes_us) / 1000 : -1;
  std::array<size_t, kNumero> orden{};
  std::iota(orden.begin(), orden.end(), size_t{0});
  std::sort(orden.begin(), orden.end(),
            [](size_t a, size_t b) { return g_cuentas[a].propio_ns > g_cuentas[b].propio_ns; });
  std::string texto;
  for (size_t i : orden) {
    const Cuenta& c = g_cuentas[i];
    if (c.llamadas == 0) {
      continue;
    }
    texto += fmt::format("{} ({}) {} llamadas, propio {:.1f} ms, inclusivo {:.1f} ms, maximo propio {:.1f} ms en "
                         "500 ms; ",
                         kFunciones[i].nombre, kFunciones[i].papel, c.llamadas, double(c.propio_ns) / 1e6,
                         double(c.inclusivo_ns) / 1e6, double(c.max_propio_ventana_ns) / 1e6);
  }
  const double segundos = double(ahora - g_desde_ns) / 1e9;
  REXLOG_INFO("[audio] funciones del servidor en {:.1f} s, CPU del hilo {} ms, tiempo propio medido {:.1f} ms: {}",
              segundos, cpu_ms, double(g_medido_ns) / 1e6, texto);
  REXLOG_INFO("[audio] llamada virtual de sub_825EB0E8 en {:.1f} s: {}", segundos, g_destinos_voz.Texto());
  REXLOG_INFO("[audio] efecto de sub_825DCED8 (puntero en 0x{:08X}) en {:.1f} s: {}", kPunteroEfecto, segundos,
              g_destinos_efecto.Texto());
  REXLOG_INFO("[audio] lista de sub_825D24E0 (0x{:08X}) en {:.1f} s: {}", kCabezaLista, segundos,
              g_destinos_lista.Texto());
  REXLOG_INFO("[audio] cola de sub_825DA1E0 (0x{:08X}) en {:.1f} s: {}", kColaOrdenes, segundos,
              g_destinos_cola.Texto());
  g_cuentas = {};
  g_destinos_voz = {};
  g_destinos_efecto = {};
  g_destinos_lista = {};
  g_destinos_cola = {};
  g_medido_ns = 0;
  g_desde_ns = ahora;
  g_ventana_desde_ns = ahora;
  g_cpu_antes_us = cpu_us;
}

// When a measured call that is not nested in another one ends: 500 ms windows and 10 s summary.
void AlSalir(int64_t ahora, int64_t total_ns) {
  g_medido_ns += total_ns;
  if (g_desde_ns == 0) {
    g_desde_ns = ahora;
    g_ventana_desde_ns = ahora;
    g_cpu_antes_us = nfsmw::hilos::CpuHiloUs(g_manejador);
    return;
  }
  if (ahora - g_ventana_desde_ns >= kVentanaNs) {
    CerrarVentana();
    g_ventana_desde_ns = ahora;
  }
  if (ahora - g_desde_ns >= kResumenNs) {
    Informar(ahora);
  }
}

}  // namespace

// Measures a call if the diagnostic is on and this is the server thread. The measurements in progress form a
// stack through the pointer to the outer one: each passes its total time to its parent, which subtracts it
// from its own self time.
class Medida {
 public:
  explicit Medida(size_t indice)
      : indice_(indice), activa_(t_servidor && REXCVAR_GET(nfsmw_audio_diag_funciones)) {
    if (!activa_) {
      return;
    }
    madre_ = actual_;
    actual_ = this;
    externa_ = g_activas[indice_]++ == 0;
    inicio_ns_ = AhoraNs();
  }
  ~Medida() {
    if (!activa_) {
      return;
    }
    const int64_t fin = AhoraNs();
    const int64_t total = fin - inicio_ns_;
    const int64_t propio = total - hijas_ns_;
    Cuenta& c = g_cuentas[indice_];
    ++c.llamadas;
    c.propio_ns += propio;
    c.propio_ventana_ns += propio;
    if (externa_) {
      c.inclusivo_ns += total;
    }
    --g_activas[indice_];
    actual_ = madre_;
    if (madre_ != nullptr) {
      madre_->hijas_ns_ += total;
    } else {
      AlSalir(fin, total);
    }
  }
  Medida(const Medida&) = delete;
  Medida& operator=(const Medida&) = delete;

  bool activa() const { return activa_; }

 private:
  static inline Medida* actual_ = nullptr;  // only the server thread touches it
  size_t indice_;
  bool activa_;
  bool externa_ = false;
  Medida* madre_ = nullptr;
  int64_t inicio_ns_ = 0;
  int64_t hijas_ns_ = 0;
};

// Called by the audio server thread's wait, once, from that thread.
void MarcarHiloServidor() {
  t_servidor = true;
  g_manejador = nfsmw::hilos::ManejadorHiloActual();
}

}  // namespace nfsmw::audio_perfil

#define NFSMW_MEDIR_FUNCION(nombre, indice)                          \
  REX_EXTERN(__imp__##nombre);                                       \
  REX_HOOK_RAW(nombre) {                                             \
    nfsmw::audio_perfil::Medida medida(nfsmw::audio_perfil::indice); \
    __imp__##nombre(ctx, base);                                      \
  }

NFSMW_MEDIR_FUNCION(sub_825CF780, kOrdenes)
NFSMW_MEDIR_FUNCION(sub_825E4160, kBucle825E4160)
NFSMW_MEDIR_FUNCION(sub_825D07C8, kBucle825D07C8)
NFSMW_MEDIR_FUNCION(sub_825CFCC8, kGrafo825CFCC8)
NFSMW_MEDIR_FUNCION(sub_825ED350, kMezclaPaquete)
NFSMW_MEDIR_FUNCION(sub_825DD288, k825DD288)
// sub_825DCED8 also records where the effects pointer points.
REX_EXTERN(__imp__sub_825DCED8);
REX_HOOK_RAW(sub_825DCED8) {
  nfsmw::audio_perfil::Medida medida(nfsmw::audio_perfil::k825DCED8);
  if (medida.activa()) {
    nfsmw::audio_perfil::AnotarDestinoEfecto(base);
  }
  __imp__sub_825DCED8(ctx, base);
}
NFSMW_MEDIR_FUNCION(sub_825D2538, k825D2538)
NFSMW_MEDIR_FUNCION(sub_825ED568, k825ED568)
NFSMW_MEDIR_FUNCION(sub_825DBD68, k825DBD68)
NFSMW_MEDIR_FUNCION(sub_825DCCB0, k825DCCB0)
NFSMW_MEDIR_FUNCION(sub_825DC0C0, k825DC0C0)
// The voice also records where its virtual call goes.
REX_EXTERN(__imp__sub_825EB0E8);
REX_HOOK_RAW(sub_825EB0E8) {
  nfsmw::audio_perfil::Medida medida(nfsmw::audio_perfil::kVoz);
  if (medida.activa()) {
    nfsmw::audio_perfil::AnotarDestinoVoz(base, ctx.r3.u32, ctx.r5.u32);
  }
  __imp__sub_825EB0E8(ctx, base);
}
NFSMW_MEDIR_FUNCION(sub_82602BE0, kRemuestreo)
// The two linear resamplers can run in native code (nfsmw_audio_remuestreo_nativo): their hook calls the
// selection in nfsmw_audio_remuestreo.cpp instead of going straight to the recompiled function. A single hook
// per function: on the Switch the linker accepts duplicate definitions (--allow-multiple-definition) and would
// silently keep one of them.
namespace nfsmw::audio_remuestreo {
void Remuestreo826031C0(PPCContext& ctx, uint8_t* base);
void Remuestreo82619820(PPCContext& ctx, uint8_t* base);
}  // namespace nfsmw::audio_remuestreo

#define NFSMW_MEDIR_FUNCION_POR(nombre, indice, llamada)            \
  REX_HOOK_RAW(nombre) {                                             \
    nfsmw::audio_perfil::Medida medida(nfsmw::audio_perfil::indice); \
    llamada(ctx, base);                                              \
  }

NFSMW_MEDIR_FUNCION_POR(sub_826031C0, kRemuestreoLineal, nfsmw::audio_remuestreo::Remuestreo826031C0)
NFSMW_MEDIR_FUNCION(sub_826033A0, kRemuestreo826033A0)
NFSMW_MEDIR_FUNCION(sub_826022C8, k826022C8)
NFSMW_MEDIR_FUNCION(sub_826047B0, k826047B0)
NFSMW_MEDIR_FUNCION(sub_8262E220, k8262E220)
NFSMW_MEDIR_FUNCION_POR(sub_82619820, k82619820, nfsmw::audio_remuestreo::Remuestreo82619820)
NFSMW_MEDIR_FUNCION(sub_82612270, k82612270)
NFSMW_MEDIR_FUNCION(sub_825E1370, k825E1370)
namespace nfsmw::audio_filtro {
void Filtro825CD088(PPCContext& ctx, uint8_t* base);  // nfsmw_audio_filtro.cpp (nfsmw_audio_filtro_nativo)
}  // namespace nfsmw::audio_filtro
NFSMW_MEDIR_FUNCION_POR(sub_825CD088, k825CD088, nfsmw::audio_filtro::Filtro825CD088)
// sub_826BDD90 (the real memcpy of the CRT, 950 callers) now goes through [rexcrt] memmove in overrides.toml
// and no longer exists in the generated code: its measurement has nothing to hook.
// NFSMW_MEDIR_FUNCION(sub_826BDD90, kAyuda826BDD90)
NFSMW_MEDIR_FUNCION(sub_82601A08, k82601A08)
NFSMW_MEDIR_FUNCION(sub_825D05F8, k825D05F8)
// What the voice and the per-packet mix call without being measured.
NFSMW_MEDIR_FUNCION(sub_825DE890, k825DE890)
NFSMW_MEDIR_FUNCION(sub_826027F0, k826027F0)
NFSMW_MEDIR_FUNCION(sub_825F2F60, k825F2F60)
NFSMW_MEDIR_FUNCION(sub_825ED268, k825ED268)
// sub_825D24E0 and sub_825DA1E0 call through pointers; their targets are recorded.
REX_EXTERN(__imp__sub_825D24E0);
REX_HOOK_RAW(sub_825D24E0) {
  nfsmw::audio_perfil::Medida medida(nfsmw::audio_perfil::k825D24E0);
  if (medida.activa()) {
    nfsmw::audio_perfil::AnotarDestinosLista(base);
  }
  __imp__sub_825D24E0(ctx, base);
}
REX_EXTERN(__imp__sub_825DA1E0);
REX_HOOK_RAW(sub_825DA1E0) {
  nfsmw::audio_perfil::Medida medida(nfsmw::audio_perfil::k825DA1E0);
  if (medida.activa()) {
    nfsmw::audio_perfil::AnotarDestinosCola(base);
  }
  __imp__sub_825DA1E0(ctx, base);
}
// The gain-scaled sum can run in native code (nfsmw_audio_suma_nativa, nfsmw_audio_suma.cpp).
namespace nfsmw::audio_suma {
void Suma825FDFB0(PPCContext& ctx, uint8_t* base);
}  // namespace nfsmw::audio_suma
NFSMW_MEDIR_FUNCION_POR(sub_825FDFB0, k825FDFB0, nfsmw::audio_suma::Suma825FDFB0)
NFSMW_MEDIR_FUNCION(sub_825E1CD0, k825E1CD0)
