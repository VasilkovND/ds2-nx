// nfsmw - measurement probe for the engine sound (Ginsu): the acceleration sound that is slow to come back
// after braking
//
// On the console, in one particular corner, after braking hard and accelerating again, the engine's acceleration
// sound takes about 2.5 s to come back; the deceleration sound does play. It happens on both laps in the same
// corner and coincides with a zone load. This probe only measures: each hook calls the original function without
// touching its registers and then reads guest memory. It does not change the audio.
//
// How the engine sounds, as seen in the PowerPC of the recompiled code:
//  - Each .gin (signature "Gnsu2", checked byte by byte in sub_8220A3A0) is entirely in RAM: sub_821F6800 looks
//    it up by name in the resource table at 0x82A31370 (104-byte entries) and returns a pointer. Synthesis
//    does not read the disc.
//  - Each .gin has a 68-byte synthesizer (vtable 0x82072490) with two buffers of rate x 0.011 samples
//    (11 ms) in a 2620-byte workspace (sub_8220B110). It registers with the game's sound engine as a
//    user stream with the callback sub_8220ABF8 (sub_825D9768) and starts its voice with priority 101
//    (sub_8220B378 -> sub_825D98B0).
//  - When the voice consumes a buffer, the audio server thread (sub_825ED350, per packet) drains the queue
//    sub_825DA1E0, which calls sub_8220ABF8(r3 = buffer, r4 = synthesizer) -> sub_8220AC08 through a pointer.
//    That function refills the buffer by decoding the .gin from RAM (sub_8220AB10 and sub_8220A138, 32-sample
//    blocks) and queues it again (sub_825D9C58).
//  - If the voice asks for more samples than are queued, sub_825FDC38 gives it nothing for that packet: with
//    no consumption there is no callback, and with no callback there is no new buffer.
//  - Per frame, CARSFX_DualGinsuEng (vtable 0x82072498) runs sub_821F69E0 and, through its method +64,
//    sub_821F74B0: Ac voice volume = (gain [ctl+60] x DMX) >> 23 and Dc voice volume = ([ctl+64] x DMX) >> 23,
//    from 0 to 127; and it sets the rpm of both synthesizers (sub_8220B558). ctl = [this+64] is the controller
//    computed by sub_821CF0D8 -> sub_821CF770: rpm at +184, rpm delta per update at +172 and its moving average
//    at +144, mix weights at +208 and +212, and the AEMS, Ac and Dc gains at +56, +60 and +64.
//
// Hooks. All three are only called through pointers: the indirect call table in nfsmw_init.cpp sees the hook
// even though tools/llamadas_directas.py has put __imp__ on the direct calls (the patch checks this).
//  - sub_8220ABF8, on the audio server thread: per synthesizer it counts buffers, samples, silent buffers, the
//    peak of the last buffer, the time of the last one and gaps longer than 50 ms. Atomics only; it does not log.
//  - sub_821F74B0 (dual engine) and sub_821F7118 (single engine), on the thread that updates the car sound:
//    they read the state, watch for gaps and write the log. [motor] lines:
//      SIN MUESTRAS  a voice has gone over 50 ms without refilling a buffer while other voices keep refilling;
//      VUELVE        the voice refills again, with the exact duration of the gap;
//      Ac apagado    a stretch of 300 ms or more with the Ac voice below a quarter of the Dc, with a trace every
//                    100 ms starting 1 s before (to compare the bad corner with the good braking events);
//      la actualizacion no corrio   more than 100 ms between two engine updates;
//      resumen       every 10 s.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"  // deferred reports

#include "nfsmw_audio_nativo.h"

REXCVAR_DEFINE_BOOL(nfsmw_audio_sonda_motor, true, "NFSMW",
                    "Sonda de medida del sonido del motor (Ginsu): buferes que rellena cada sintetizador, volumenes, "
                    "rpm y estado de las voces; anota en el log los huecos de mas de 50 ms y los tramos con la "
                    "aceleracion apagada. Solo mide: no cambia el audio");

namespace nfsmw::audio_motor_sonda {
namespace {

using nfsmw::audio_nativo::Dir;
using nfsmw::audio_nativo::Leer16;
using nfsmw::audio_nativo::Leer32;
using nfsmw::audio_nativo::LeerFloat;

// The game's sound engine (sub_825D9768, sub_825D9C58, sub_825DD8B0, sub_825FBC68, sub_825FDC38).
constexpr uint32_t kTablaFlujos = 0x82A2AD38 + 772;  // pointers to the user streams, by index
constexpr uint32_t kSnd = 0x82A2B1D0;  // +14 number of streams (byte), +48 number of voices (16 bits), +136 voices
constexpr uint32_t kTamVoz = 132;      // voice: +0 handle, +4 first physical voice, +56 volume, +105 in use,
                                       // +128 tono

constexpr int64_t kHuecoNs = 50'000'000;               // 50 ms without refilling a buffer...
constexpr uint64_t kOtrosMinimos = 3;                  // ...while other voices refill at least 3
constexpr int64_t kActualizacionParadaNs = 100'000'000;
constexpr int64_t kResumenNs = 10'000'000'000;
constexpr int64_t kTrazaCadaNs = 100'000'000;          // one trace sample every 100 ms
constexpr size_t kTraza = 64;                          // 6,4 s de traza
constexpr int64_t kTrazaAntesNs = 1'000'000'000;       // the trace of a stretch starts 1 s before
constexpr int64_t kApagadoMinimoNs = 300'000'000;      // stretches with the Ac voice off that get logged
constexpr int64_t kApagadoLargoNs = 4'000'000'000;     // aviso en marcha si no vuelve
constexpr int64_t kOlvidarNs = 60'000'000'000;         // an inactive synthesizer or engine is forgotten
constexpr int32_t kPicoMudo = 16;                      // silent buffer: peak below 16 out of 32767
constexpr uint32_t kMaxMuestras = 552;                 // buffer limit (sub_8220B0E0)

int64_t AhoraNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// Guest pointers that can be followed (heap or image objects, non-null).
bool Valido(uint32_t dir) {
  return dir >= 0x10000 && dir < 0xF0000000u;
}

size_t IdHilo() {
  thread_local const size_t id = std::hash<std::thread::id>{}(std::this_thread::get_id());
  return id;
}

// What the server thread records on each synthesizer callback. Atomics only: it is read by the thread that
// updates the car sound.
struct Sintetizador {
  std::atomic<uint32_t> direccion{0};
  std::atomic<int64_t> ultima_ns{0};
  std::atomic<uint64_t> secuencia{0};  // value of g_secuencia at its last buffer
  std::atomic<uint64_t> buferes{0};
  std::atomic<uint64_t> muestras{0};
  std::atomic<uint64_t> mudos{0};
  std::atomic<int32_t> pico{0};  // of the last buffer
  std::atomic<int64_t> hueco_max_ns{0};
  // The last gap longer than 50 ms, measured when the voice refills again.
  std::atomic<uint64_t> huecos{0};
  std::atomic<int64_t> hueco_ns{0};
  std::atomic<uint64_t> hueco_otros{0};  // buffers from other voices during the gap
};

std::array<Sintetizador, 16> g_sint;
std::atomic<uint64_t> g_secuencia{0};
std::atomic<size_t> g_hilo_retrollamadas{0};

Sintetizador* Buscar(uint32_t s, bool crear) {
  if (!Valido(s)) {
    return nullptr;
  }
  for (Sintetizador& e : g_sint) {
    if (e.direccion.load(std::memory_order_acquire) == s) {
      return &e;
    }
  }
  if (!crear) {
    return nullptr;
  }
  for (Sintetizador& e : g_sint) {
    uint32_t libre = 0;
    if (e.direccion.compare_exchange_strong(libre, s, std::memory_order_acq_rel)) {
      return &e;
    }
  }
  return nullptr;  // table full: that synthesizer is not measured
}

// Audio server thread, after the original callback has refilled the buffer.
void AlRellenar(uint8_t* base, uint32_t bufer, uint32_t s) {
  Sintetizador* e = Buscar(s, true);
  if (e == nullptr) {
    return;
  }
  size_t sin_hilo = 0;
  g_hilo_retrollamadas.compare_exchange_strong(sin_hilo, IdHilo());
  const int64_t ahora = AhoraNs();
  const uint64_t secuencia = g_secuencia.fetch_add(1) + 1;
  const int64_t antes = e->ultima_ns.exchange(ahora);
  const uint64_t secuencia_antes = e->secuencia.exchange(secuencia);
  if (antes != 0) {
    const int64_t hueco = ahora - antes;
    if (hueco > e->hueco_max_ns.load(std::memory_order_relaxed)) {
      e->hueco_max_ns.store(hueco, std::memory_order_relaxed);
    }
    if (hueco > kHuecoNs) {
      e->hueco_ns.store(hueco);
      e->hueco_otros.store(secuencia - secuencia_antes - 1);
      e->huecos.fetch_add(1);
    }
  }
  // Peak of the freshly refilled buffer: [s+20] 16-bit big-endian samples.
  const uint32_t n = Leer32(base, s + 20);
  int32_t pico = 0;
  if (n > 0 && n <= kMaxMuestras && Valido(bufer)) {
    const uint8_t* p = Dir(base, bufer);
    for (uint32_t i = 0; i < n; ++i) {
      const int16_t v = static_cast<int16_t>(static_cast<uint16_t>((p[2 * i] << 8) | p[2 * i + 1]));
      pico = std::max<int32_t>(pico, std::abs(static_cast<int32_t>(v)));
    }
  }
  e->pico.store(pico, std::memory_order_relaxed);
  e->buferes.fetch_add(1, std::memory_order_relaxed);
  e->muestras.fetch_add(n, std::memory_order_relaxed);
  if (pico < kPicoMudo) {
    e->mudos.fetch_add(1, std::memory_order_relaxed);
  }
}

// One engine voice (slot 0 = Ac, slot 1 = Dc), read from the engine object, its synthesizer, its user
// stream and the sound engine voice.
struct Voz {
  uint32_t sint = 0;
  uint32_t manejador = 0;    // [this+104] o [this+128]
  int32_t volumen = 0;       // [this+136] o [this+140], 0..127
  bool valida = false;       // the same check as sub_825DD8B0
  int32_t tono = 0;          // [voz+128]
  float volumen_snd = -1.0f; // [voz fisica+56] (inferido de sub_825D93D0)
  uint32_t tasa = 0;         // [s+36]; 0 = the synthesizer has the voice stopped (sub_8220B6A0)
  uint32_t n = 0;            // [s+20], samples per buffer
  uint32_t pos = 0;          // [s+44], read pointer in the .gin
  uint32_t total = 0;        // [[s+40]+20], samples of the .gin
  int32_t actual = 0;        // [s+48]
  int32_t destino = 0;       // [s+52], set by sub_8220B558 from the rpm
  int32_t pasos = 0;         // [s+56]
  int32_t flujo = -1;        // [s+28]
  uint32_t voz_flujo = 0;    // [flujo+0]
  int32_t en_cola = 0;       // [flujo+72], muestras encoladas
  int32_t arrastre = 0;      // [flujo+76]
  int32_t buferes_cola = 0;  // [flujo+52]
};

struct Estado {
  bool doble = false;
  uint32_t listo = 0;   // [this+76]
  uint32_t activo = 0;  // [this+8]
  int32_t dmx = 0;      // [[this+16]+4] & 0x7FFF
  uint32_t ctl = 0;     // [this+64]
  float rpm = 0.0f, delta = 0.0f, delta_media = 0.0f, peso_a = 0.0f, peso_b = 0.0f;
  int32_t g_aems = 0, g_ac = 0, g_dc = 0;
  std::array<Voz, 2> voz{};
};

Voz LeerVoz(uint8_t* base, uint32_t s, uint32_t manejador, int32_t volumen) {
  Voz v;
  v.sint = s;
  v.manejador = manejador;
  v.volumen = volumen;
  if (static_cast<int32_t>(manejador) >= 0) {
    const uint32_t indice = manejador & 0xFF;
    const uint32_t cuantas = Leer16(base, kSnd + 48);
    const uint32_t tabla = Leer32(base, kSnd + 136);
    if (indice < cuantas && Valido(tabla)) {
      const uint32_t voz = tabla + indice * kTamVoz;
      v.valida = *Dir(base, voz + 105) != 0 && Leer32(base, voz) == manejador;
      v.tono = static_cast<int16_t>(Leer16(base, voz + 128));
      const uint32_t fisica = Leer16(base, voz + 4);
      if (fisica < cuantas) {
        v.volumen_snd = LeerFloat(base, tabla + fisica * kTamVoz + 56);
      }
    }
  }
  if (!Valido(s)) {
    return v;
  }
  v.tasa = Leer32(base, s + 36);
  v.n = Leer32(base, s + 20);
  v.pos = Leer32(base, s + 44);
  v.actual = static_cast<int32_t>(Leer32(base, s + 48));
  v.destino = static_cast<int32_t>(Leer32(base, s + 52));
  v.pasos = static_cast<int32_t>(Leer32(base, s + 56));
  const uint32_t datos = Leer32(base, s + 40);
  if (Valido(datos)) {
    v.total = Leer32(base, datos + 20);
  }
  v.flujo = static_cast<int32_t>(Leer32(base, s + 28));
  if (v.flujo >= 0 && v.flujo < static_cast<int32_t>(*Dir(base, kSnd + 14))) {
    const uint32_t f = Leer32(base, kTablaFlujos + static_cast<uint32_t>(v.flujo) * 4);
    if (Valido(f)) {
      v.voz_flujo = Leer32(base, f + 0);
      v.en_cola = static_cast<int32_t>(Leer32(base, f + 72));
      v.arrastre = static_cast<int32_t>(Leer32(base, f + 76));
      v.buferes_cola = static_cast<int16_t>(Leer16(base, f + 52));
    }
  }
  return v;
}

Estado LeerEstado(uint8_t* base, uint32_t objeto, bool doble) {
  Estado e;
  e.doble = doble;
  e.listo = *Dir(base, objeto + 76);
  e.activo = *Dir(base, objeto + 8);
  const uint32_t entradas = Leer32(base, objeto + 16);
  if (Valido(entradas)) {
    e.dmx = static_cast<int32_t>(Leer32(base, entradas + 4) & 0x7FFF);
  }
  e.ctl = Leer32(base, objeto + 64);
  if (Valido(e.ctl)) {
    e.rpm = LeerFloat(base, e.ctl + 184);
    e.delta = LeerFloat(base, e.ctl + 172);
    e.delta_media = LeerFloat(base, e.ctl + 144);
    e.peso_a = LeerFloat(base, e.ctl + 208);
    e.peso_b = LeerFloat(base, e.ctl + 212);
    e.g_aems = static_cast<int32_t>(Leer32(base, e.ctl + 56));
    e.g_ac = static_cast<int32_t>(Leer32(base, e.ctl + 60));
    e.g_dc = static_cast<int32_t>(Leer32(base, e.ctl + 64));
  }
  e.voz[0] = LeerVoz(base, Leer32(base, objeto + 88), Leer32(base, objeto + 104),
                     static_cast<int32_t>(Leer32(base, objeto + 136)));
  if (doble) {
    e.voz[1] = LeerVoz(base, Leer32(base, objeto + 112), Leer32(base, objeto + 128),
                       static_cast<int32_t>(Leer32(base, objeto + 140)));
  }
  return e;
}

std::string TextoVoz(const char* nombre, const Voz& v, int64_t ahora) {
  std::string rellenos = "sin retrollamadas vistas";
  if (const Sintetizador* s = Buscar(v.sint, false)) {
    const int64_t ultima = s->ultima_ns.load();
    rellenos = fmt::format("{} buferes, ultimo hace {:.1f} ms con pico {}", s->buferes.load(),
                           ultima != 0 ? double(ahora - ultima) / 1e6 : -1.0, s->pico.load());
  }
  return fmt::format("{}: vol {} (SND {:.3f}), sint 0x{:08X} {} (tasa {} Hz, {} muestras por bufer), {}; "
                     ".gin pos {} de {}, actual {} destino {} pasos {}; voz 0x{:08X} {} tono {}; flujo {} "
                     "(voz 0x{:08X}) con {} muestras en cola + {} y {} buferes",
                     nombre, v.volumen, v.volumen_snd, v.sint, v.tasa != 0 ? "arrancado" : "PARADO", v.tasa, v.n,
                     rellenos, v.pos, v.total, v.actual, v.destino, v.pasos, v.manejador,
                     v.valida ? "valida" : "NO VALIDA", v.tono, v.flujo, v.voz_flujo, v.en_cola, v.arrastre,
                     v.buferes_cola);
}

std::string TextoEstado(const Estado& e, int64_t ahora) {
  std::string t = fmt::format("DMX {}, listo {}, activo {}; ctl 0x{:08X}: rpm {:.0f}, delta {:.1f} (media {:.1f}), "
                              "pesos {:.2f}/{:.2f}, ganancias AEMS {} Ac {} Dc {}; ",
                              e.dmx, e.listo, e.activo, e.ctl, e.rpm, e.delta, e.delta_media, e.peso_a, e.peso_b,
                              e.g_aems, e.g_ac, e.g_dc);
  t += TextoVoz("Ac (ranura 0)", e.voz[0], ahora);
  if (e.doble) {
    t += "; ";
    t += TextoVoz("Dc (ranura 1)", e.voz[1], ahora);
  }
  return t;
}

struct Muestra {
  int64_t ns = 0;
  int32_t vol_ac = 0, vol_dc = 0;
  float rpm = 0.0f, delta_media = 0.0f;
  int32_t g_ac = 0, g_dc = 0;
  uint64_t buferes_ac = 0, buferes_dc = 0;
};

// State of one engine (normally the player's car). Only the thread that updates it touches it, under g_cerrojo.
struct Motor {
  uint32_t objeto = 0;
  bool doble = false;
  bool presentado = false;
  int64_t visto_ns = 0;
  int64_t ultima_ns = 0;
  uint64_t actualizaciones = 0;
  int64_t hueco_max_ns = 0;
  bool en_mismo_hilo = false;
  std::array<uint32_t, 2> sint{};  // Ac and Dc synthesizers from the last update
  std::array<bool, 2> en_hueco{};
  std::array<uint64_t, 2> huecos_vistos{};
  // Counters of each synthesizer at the previous summary (reset if the synthesizer changes).
  std::array<uint32_t, 2> sint_antes{};
  std::array<uint64_t, 2> buferes_antes{}, muestras_antes{}, mudos_antes{}, huecos_antes{};
  uint64_t paradas_servidor = 0;  // gaps in which the other voices did not refill either
  bool apagado = false;
  bool apagado_avisado = false;
  int64_t apagado_desde_ns = 0;
  float rpm_al_apagar = 0.0f;
  int32_t vol_dc_max = 0;
  std::array<Muestra, kTraza> traza{};
  size_t traza_n = 0;
  int64_t traza_ultima_ns = 0;
};

std::mutex g_cerrojo;
std::array<Motor, 8> g_motores;
int64_t g_resumen_desde_ns = 0;

Motor* MotorDe(uint32_t objeto, bool doble, int64_t ahora) {
  Motor* libre = nullptr;
  for (Motor& m : g_motores) {
    if (m.objeto == objeto) {
      return &m;
    }
    if (libre == nullptr && (m.objeto == 0 || ahora - m.visto_ns > kOlvidarNs)) {
      libre = &m;
    }
  }
  if (libre != nullptr) {
    *libre = Motor{};
    libre->objeto = objeto;
    libre->doble = doble;
  }
  return libre;
}

std::string TextoTraza(const Motor& m, int64_t desde_ns) {
  std::string t;
  const size_t n = std::min(m.traza_n, kTraza);
  for (size_t i = m.traza_n - n; i < m.traza_n; ++i) {
    const Muestra& mu = m.traza[i % kTraza];
    if (mu.ns < desde_ns) {
      continue;
    }
    t += fmt::format("{}{}:{}/{} {:.0f} {:.1f} {}/{} {}/{}", t.empty() ? "" : " | ",
                     (mu.ns - m.apagado_desde_ns) / 1'000'000, mu.vol_ac, mu.vol_dc, mu.rpm, mu.delta_media, mu.g_ac,
                     mu.g_dc, mu.buferes_ac, mu.buferes_dc);
  }
  return t;
}

// One line per engine seen in the last 10 s, with what each synthesizer refilled in that time.
void Resumir(Motor& m, int64_t periodo_ns) {
  std::string texto;
  const int ranuras = m.doble ? 2 : 1;
  for (int k = 0; k < ranuras; ++k) {
    if (m.sint[k] != m.sint_antes[k]) {
      m.sint_antes[k] = m.sint[k];
      m.buferes_antes[k] = m.muestras_antes[k] = m.mudos_antes[k] = m.huecos_antes[k] = 0;
    }
    Sintetizador* s = Buscar(m.sint[k], false);
    if (s == nullptr) {
      texto += fmt::format("; {}: sin retrollamadas vistas", k == 0 ? "Ac" : "Dc");
      continue;
    }
    const uint64_t buferes = s->buferes.load();
    const uint64_t muestras = s->muestras.load();
    const uint64_t mudos = s->mudos.load();
    const uint64_t huecos = s->huecos.load();
    texto += fmt::format("; {} (sint 0x{:08X}): {} buferes, {} muestras, {} mudos, hueco maximo {:.1f} ms, {} huecos "
                         "de mas de 50 ms",
                         k == 0 ? "Ac" : "Dc", m.sint[k], buferes - m.buferes_antes[k], muestras - m.muestras_antes[k],
                         mudos - m.mudos_antes[k], double(s->hueco_max_ns.exchange(0)) / 1e6,
                         huecos - m.huecos_antes[k]);
    m.buferes_antes[k] = buferes;
    m.muestras_antes[k] = muestras;
    m.mudos_antes[k] = mudos;
    m.huecos_antes[k] = huecos;
  }
  NFSMW_INFORME_DIFERIDO("[motor] resumen de {:.1f} s: motor 0x{:08X} ({}) {} actualizaciones, hueco maximo {:.1f} ms, {}en el "
              "hilo de las retrollamadas, {} huecos con todas las voces paradas{}",
              double(periodo_ns) / 1e9, m.objeto, m.doble ? "doble" : "simple", m.actualizaciones,
              double(m.hueco_max_ns) / 1e6, m.en_mismo_hilo ? "" : "no ", m.paradas_servidor, texto);
  m.actualizaciones = 0;
  m.hueco_max_ns = 0;
}

// Thread that updates the car sound, after the original engine update.
void AlActualizar(uint8_t* base, uint32_t objeto, bool doble) {
  if (!Valido(objeto)) {
    return;
  }
  const int64_t ahora = AhoraNs();
  std::lock_guard<std::mutex> cerrojo(g_cerrojo);
  Motor* m = MotorDe(objeto, doble, ahora);
  if (m == nullptr) {
    return;
  }
  m->visto_ns = ahora;
  ++m->actualizaciones;
  m->en_mismo_hilo = IdHilo() == g_hilo_retrollamadas.load();
  const Estado e = LeerEstado(base, objeto, doble);
  m->sint[0] = e.voz[0].sint;
  m->sint[1] = e.voz[1].sint;

  if (!m->presentado && e.listo != 0) {
    m->presentado = true;
    REXLOG_INFO("[motor] motor 0x{:08X} ({}) listo; {}", objeto, doble ? "doble" : "simple", TextoEstado(e, ahora));
  }

  // 1. The engine update itself.
  if (m->ultima_ns != 0) {
    const int64_t hueco = ahora - m->ultima_ns;
    m->hueco_max_ns = std::max(m->hueco_max_ns, hueco);
    if (hueco > kActualizacionParadaNs) {
      REXLOG_INFO("[motor] la actualizacion del motor 0x{:08X} no corrio en {:.1f} ms; {}", objeto,
                  double(hueco) / 1e6, TextoEstado(e, ahora));
    }
  }
  m->ultima_ns = ahora;

  // 2. Gaps of each voice: no buffer refill while the other voices refill.
  const int ranuras = doble ? 2 : 1;
  for (int k = 0; k < ranuras; ++k) {
    const char* nombre = k == 0 ? "Ac (ranura 0)" : "Dc (ranura 1)";
    const Sintetizador* s = Buscar(e.voz[k].sint, false);
    if (s == nullptr) {
      continue;
    }
    const int64_t ultima = s->ultima_ns.load();
    const uint64_t otros = g_secuencia.load() - s->secuencia.load();
    if (!m->en_hueco[k] && ultima != 0 && ahora - ultima > kHuecoNs && otros >= kOtrosMinimos) {
      m->en_hueco[k] = true;
      REXLOG_INFO("[motor] SIN MUESTRAS {}: {:.1f} ms sin rellenar bufer mientras otras voces rellenaron {}; {}",
                  nombre, double(ahora - ultima) / 1e6, otros, TextoEstado(e, ahora));
    }
    const uint64_t huecos = s->huecos.load();
    if (huecos != m->huecos_vistos[k]) {
      m->huecos_vistos[k] = huecos;
      const uint64_t otros_hueco = s->hueco_otros.load();
      if (m->en_hueco[k] || otros_hueco >= kOtrosMinimos) {
        REXLOG_INFO("[motor] VUELVE {} tras {:.1f} ms sin rellenar (otras voces rellenaron {} buferes); {}", nombre,
                    double(s->hueco_ns.load()) / 1e6, otros_hueco, TextoEstado(e, ahora));
      } else {
        ++m->paradas_servidor;
      }
      m->en_hueco[k] = false;
    }
  }

  // 3. Trace every 100 ms and stretches with the Ac voice off while the Dc plays (dual engine only).
  if (doble) {
    if (ahora - m->traza_ultima_ns >= kTrazaCadaNs) {
      m->traza_ultima_ns = ahora;
      Muestra& mu = m->traza[m->traza_n % kTraza];
      mu.ns = ahora;
      mu.vol_ac = e.voz[0].volumen;
      mu.vol_dc = e.voz[1].volumen;
      mu.rpm = e.rpm;
      mu.delta_media = e.delta_media;
      mu.g_ac = e.g_ac;
      mu.g_dc = e.g_dc;
      const Sintetizador* ac = Buscar(e.voz[0].sint, false);
      const Sintetizador* dc = Buscar(e.voz[1].sint, false);
      mu.buferes_ac = ac != nullptr ? ac->buferes.load() : 0;
      mu.buferes_dc = dc != nullptr ? dc->buferes.load() : 0;
      ++m->traza_n;
    }
    const bool apagado = e.listo != 0 && e.dmx > 0 && e.voz[0].volumen * 4 < e.voz[1].volumen;
    if (apagado && !m->apagado) {
      m->apagado = true;
      m->apagado_avisado = false;
      m->apagado_desde_ns = ahora;
      m->rpm_al_apagar = e.rpm;
      m->vol_dc_max = 0;
    }
    if (apagado) {
      m->vol_dc_max = std::max(m->vol_dc_max, e.voz[1].volumen);
      if (!m->apagado_avisado && ahora - m->apagado_desde_ns > kApagadoLargoNs) {
        m->apagado_avisado = true;
        REXLOG_INFO("[motor] Ac lleva {:.0f} ms apagado con Dc sonando; {}",
                    double(ahora - m->apagado_desde_ns) / 1e6, TextoEstado(e, ahora));
      }
    } else if (m->apagado) {
      m->apagado = false;
      const int64_t duracion = ahora - m->apagado_desde_ns;
      if (duracion >= kApagadoMinimoNs) {
        REXLOG_INFO("[motor] Ac apagado {:.0f} ms con Dc sonando (vol Dc maximo {}), rpm {:.0f} -> {:.0f}; al volver: "
                    "{}; traza cada 100 ms desde 1 s antes (ms desde el inicio:vol Ac/Dc rpm delta_media "
                    "ganancia Ac/Dc buferes Ac/Dc): {}",
                    double(duracion) / 1e6, m->vol_dc_max, m->rpm_al_apagar, e.rpm, TextoEstado(e, ahora),
                    TextoTraza(*m, m->apagado_desde_ns - kTrazaAntesNs));
      }
    }
  }

  // 4. Summary every 10 s of the engines seen in that time, and forgetting dead synthesizers.
  if (g_resumen_desde_ns == 0) {
    g_resumen_desde_ns = ahora;
  } else if (ahora - g_resumen_desde_ns >= kResumenNs) {
    for (Motor& otro : g_motores) {
      if (otro.objeto != 0 && otro.actualizaciones != 0) {
        Resumir(otro, ahora - g_resumen_desde_ns);
      }
    }
    g_resumen_desde_ns = ahora;
    for (Sintetizador& s : g_sint) {
      const int64_t ultima = s.ultima_ns.load();
      if (s.direccion.load() != 0 && ultima != 0 && ahora - ultima > kOlvidarNs) {
        s.ultima_ns.store(0);
        s.buferes.store(0);
        s.muestras.store(0);
        s.mudos.store(0);
        s.huecos.store(0);
        s.direccion.store(0, std::memory_order_release);
      }
    }
  }
}

}  // namespace
}  // namespace nfsmw::audio_motor_sonda

// Callback of the Ginsu synthesizer (r3 = buffer to refill, r4 = synthesizer). Called through a pointer by
// the queue sub_825DA1E0 on the audio server thread; the original only swaps r3 and r4 and jumps to
// sub_8220AC08.
REX_EXTERN(__imp__sub_8220ABF8);
REX_HOOK_RAW(sub_8220ABF8) {
  const bool medir = REXCVAR_GET(nfsmw_audio_sonda_motor);
  const uint32_t bufer = ctx.r3.u32;
  const uint32_t sintetizador = ctx.r4.u32;
  __imp__sub_8220ABF8(ctx, base);
  if (medir) {
    nfsmw::audio_motor_sonda::AlRellenar(base, bufer, sintetizador);
  }
}

// Per-frame update of CARSFX_DualGinsuEng (method +64 of its vtable, called by sub_821F69E0).
REX_EXTERN(__imp__sub_821F74B0);
REX_HOOK_RAW(sub_821F74B0) {
  const bool medir = REXCVAR_GET(nfsmw_audio_sonda_motor);
  const uint32_t objeto = ctx.r3.u32;
  __imp__sub_821F74B0(ctx, base);
  if (medir) {
    nfsmw::audio_motor_sonda::AlActualizar(base, objeto, true);
  }
}

// The same for CARSFX_SingleGinsuEng (a single .gin; method +64 of vtable 0x820724E0).
REX_EXTERN(__imp__sub_821F7118);
REX_HOOK_RAW(sub_821F7118) {
  const bool medir = REXCVAR_GET(nfsmw_audio_sonda_motor);
  const uint32_t objeto = ctx.r3.u32;
  __imp__sub_821F7118(ctx, base);
  if (medir) {
    nfsmw::audio_motor_sonda::AlActualizar(base, objeto, false);
  }
}
