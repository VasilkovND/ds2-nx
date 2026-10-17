// nfsmw - rescue of the game's audio server thread
//
// ===========================================================================
//  THE FAILURE (observed on the console, in the menu with the gamepad)
//  The menu audio goes silent and the race never finishes loading. The
//  watchdog always dumps guest thread 0xD waiting forever in
//  KeWaitForSingleObject (lr 0x82853230) on the audio server's event,
//  with ctr = 0x8285D560: its last indirect call was the SetState of a
//  packet submission that succeeded.
//
//  THE SERVER LOOP (sub_825E3E28, recompiled code)
//  Object in r31 and a ring of 2 packets of 6144 bytes:
//    +146        active              +152        XAudio voice
//    +12444      R: next packet to submit
//    +12448      W: next packet to mix
//    +12452      event (auto-reset)
//    +12456+88*i packet i (its context, at +84, is &state[i])
//    +12632+4*i  state i: 0 free, 1 full, 2 submitted to the voice
//  1. Waits for the event at 0x825E3EF0 with sub_828531E8, with no timeout.
//  2. On wakeup, if slot W is not free, it waits again without attempting
//     any submission.
//  3. If it is free: sub_825CF780 (the game's audio commands), mixes into W
//     (state 1) and submits in order while the voice has a free node (bit 0x20
//     of sub_82851D98): sub_82851F00 and state 2.
//  4. The end of each packet arrives through sub_825E4358 (state 0 and signal)
//     from the SDK's audio thread; each pass of the voice calls sub_825E4338,
//     which only signals.
//  If the voice stops returning end-of-packet notifications, the thread keeps
//  waiting with slot W occupied: sub_825CF780 never runs again and the game's
//  audio commands, including those of the race load, go unserviced.
//
//  WHAT THIS FILE DOES
//  It replaces that wait with waits that have a timeout. While end-of-packet
//  notifications keep arriving (one every ~5 ms) the loop does exactly the
//  same. If the thread has spent 250 ms with slot W occupied and no end of
//  packet or submission:
//   - it retries the pending submissions with the same condition as the game;
//   - if slot W is still occupied, it frees it and returns, so that the loop
//     keeps mixing and servicing commands at ~5 ms per packet, albeit silent;
//   - as soon as a real end of packet arrives, it stops freeing.
//  Entering and leaving that mode is logged with the state of the packet
//  ring and of the voice.
//
//  HOW TO TURN IT OFF WITHOUT A NEW NRO
//  In nfsmw.toml:   nfsmw_audio_rescate = false
//
//  DIAGNOSTICS (off by default)
//  nfsmw_audio_diag_retraso_servidor_us delays each wakeup of the thread, to
//  see on PC what happens if it is slow to run again, as can happen on
//  Horizon. nfsmw_audio_diag_anillo logs every 10 s how many packets the
//  voice had on each pass.
//
//  ROBOTIC AUDIO IN CRASHES (measured on the console)
//  In the Ironwood Estates alley the server thread (XThread5F79080 on the
//  console) delivered 89 % of the packets: it ran at 0x3B, taking turns every
//  10 ms with Main and XThread59CC1C0, and 1,308 passes of the voice came out
//  without a packet. Defaults on the Switch:
//   - nfsmw_audio_servidor_prioridad = 0x2D: it takes the CPU from them as soon
//     as it wakes up;
//   - nfsmw_audio_esperar_servidor_ms = 30: the Audio Worker waits for the late
//     packet instead of mixing the audio frame with the voice silent, and the
//     cost is absorbed by the audio_switch_tramas_en_cola cushion (10 audio
//     frames, 53 ms).
//  On PC both stay at 0: with the whole CPU the packet ring never runs dry.
// ===========================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_BOOL(nfsmw_audio_rescate, true, "NFSMW",
                    "Mantener en marcha el hilo servidor de audio del juego si la voz de XAudio "
                    "deja de devolver fines de paquete");
REXCVAR_DEFINE_INT32(nfsmw_audio_diag_retraso_servidor_us, 0, "NFSMW",
                     "Diagnostico: retrasa N microsegundos cada despertar del hilo servidor de "
                     "audio del juego, para imitar en el PC lo que tarda en volver a correr en la "
                     "Switch; 0 = nada");
REXCVAR_DEFINE_INT32(nfsmw_audio_servidor_prioridad, REX_PLATFORM_SWITCH != 0 ? 0x2D : 0, "NFSMW",
                     "Prioridad de Horizon del hilo servidor de audio del juego (0x1C-0x3A); 0 = la de los hilos "
                     "del juego (0x3B). Por defecto 45 (0x2D) en la Switch, para que no espere su turno detras de "
                     "ellos en los choques; en el PC cualquier valor lo sube a THREAD_PRIORITY_HIGHEST");
REXCVAR_DEFINE_BOOL(nfsmw_audio_diag_anillo, false, "NFSMW",
                    "Diagnostico: cada 10 s anota cuantos paquetes tenia la voz del servidor de "
                    "audio en cada pasada, los fines de paquete y las entregas, y la CPU y los nucleos del "
                    "hilo servidor");
REXCVAR_DEFINE_INT32(nfsmw_audio_diag_anillo_ms, 10000, "NFSMW",
                     "Diagnostico: milisegundos entre resumenes de nfsmw_audio_diag_anillo (como poco 100); "
                     "500 separa los choques del atajo del callejon");
REXCVAR_DEFINE_DOUBLE(nfsmw_audio_diag_lentitud_mezcla, 0.0, "NFSMW",
                      "Diagnostico: tras cada mezcla del hilo servidor de audio espera activamente N veces lo que "
                      "ha tardado, antes de entregar el paquete; 3 imita una mezcla 4 veces mas lenta; 0 = nada");
REXCVAR_DEFINE_INT32(nfsmw_audio_esperar_servidor_ms, REX_PLATFORM_SWITCH != 0 ? 30 : 0, "NFSMW",
                     "Antes de cada trama, si la voz del servidor de audio del juego no tiene paquete y el "
                     "servidor esta en marcha, esperar como mucho N ms a que lo entregue (en vez de mezclar la "
                     "trama con esa voz en silencio); 0 = no esperar (por defecto 30 en la Switch y 0 en el PC). "
                     "Necesita colchon en el driver (audio_switch_tramas_en_cola en la Switch, "
                     "audio_sdl_bomba_cola en el PC)");

namespace nfsmw::hilos {
// nfsmw_hilos_switch.cpp
bool PrioridadHiloActual(int prioridad);
uint64_t ManejadorHiloActual();
int64_t CpuHiloUs(uint64_t manejador);
int NucleoActual();
}  // namespace nfsmw::hilos

namespace nfsmw::audio_servidor {

int64_t RelojReal() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The test bench replaces it with a simulated clock.
int64_t (*g_reloj_ms)() = &RelojReal;

namespace {

constexpr uint32_t kRetornoEspera = 0x825E3EF4;
constexpr uint32_t kRetornoEntrega = 0x825E4068;
constexpr uint32_t kRetornoArranque = 0x825E3A3C;
constexpr uint32_t kBanderaServidor = 0x82A2B2AC;
constexpr uint32_t kOffActivo = 146;
constexpr uint32_t kOffVoz = 152;
constexpr uint32_t kOffEstadoVoz = 61;
constexpr uint32_t kOffR = 12444;
constexpr uint32_t kOffW = 12448;
constexpr uint32_t kOffEvento = 12452;
constexpr uint32_t kOffPaquetes = 12456;
constexpr uint32_t kBytesPaquete = 88;
constexpr uint32_t kOffEstados = 12632;
constexpr uint32_t kLibre = 0;
constexpr uint32_t kLleno = 1;
constexpr uint32_t kEntregado = 2;
constexpr uint8_t kBitNodoLibre = 0x20;
constexpr uint32_t kStatusTimeout = 0x102;

constexpr int64_t kPlazoMs = 50;
constexpr int64_t kPlazoRescateMs = 5;
constexpr int64_t kSinAvanceMs = 250;
constexpr int64_t kResumenRescateMs = 10000;
constexpr uint64_t kMaxVolcados = 20;

uint32_t Leer32(const uint8_t* base, uint32_t direccion) {
  uint32_t v = 0;
  std::memcpy(&v, base + direccion, sizeof(v));
  return __builtin_bswap32(v);
}

void Escribir32(uint8_t* base, uint32_t direccion, uint32_t valor) {
  valor = __builtin_bswap32(valor);
  std::memcpy(base + direccion, &valor, sizeof(valor));
}

uint32_t Estado(const uint8_t* base, uint32_t obj, uint32_t i) {
  return Leer32(base, obj + kOffEstados + 4 * i);
}

// Written by the hooks of the notifications (SDK audio thread) and of the
// submission; read by the server thread.
std::atomic<uint64_t> g_fines{0};
std::atomic<uint64_t> g_pasadas{0};
std::atomic<uint64_t> g_entregas{0};
std::atomic<int64_t> g_ultimo_fin_ms{0};
std::atomic<int64_t> g_ultima_entrega_ms{0};

// nfsmw_audio_diag_anillo: from wakeup to submission. g_despertar_us is written and read by the server thread
// (its wait and its submission); the summary resets the sums, from the SDK audio thread.
std::atomic<int64_t> g_despertar_us{0};
std::atomic<uint64_t> g_mezclas{0};
std::atomic<uint64_t> g_mezcla_suma_us{0};
std::atomic<int64_t> g_mezcla_max_us{0};

// nfsmw_audio_esperar_servidor_ms: the guest memory base is recorded by the server's wait; the submission
// notifies the condition and the Audio Worker waits on it before each audio frame. The counters are reset by
// the packet ring summary.
std::atomic<uint8_t*> g_base_servidor{nullptr};
std::mutex g_entrega_mutex;
std::condition_variable g_entrega_cv;
std::atomic<uint64_t> g_esperas{0};
std::atomic<uint64_t> g_esperas_agotadas{0};
std::atomic<uint64_t> g_espera_suma_us{0};
std::atomic<int64_t> g_espera_max_us{0};
// With no submissions within this time the server is considered stopped (loads, videos): no waiting.
constexpr int64_t kServidorParadoMs = 300;

int64_t RelojUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// nfsmw_audio_diag_* diagnostics. The server object is recorded by its wait; the passes are
// recorded by the SDK audio thread, which is the only one that calls them.
std::atomic<uint32_t> g_obj_servidor{0};
// Handle of the server thread, to read its CPU time from the summary, and its wakeups per core (the core the
// system reports when the wait returns signaled; the last bucket counts any other, such as PC cores above 3).
std::atomic<uint64_t> g_manejador_servidor{0};
std::atomic<uint64_t> g_despertares_nucleo[5];
struct DiagAnillo {
  uint64_t pasadas_con[3] = {0, 0, 0};
  int64_t desde_ms = 0;
  uint64_t fines_antes = 0;
  uint64_t entregas_antes = 0;
  // The longest a pass has seen since the server's last submission: how long it takes to run again.
  int64_t max_sin_entrega_ms = 0;
  // CPU time the server thread had used when the summary started (-1 = unknown).
  int64_t cpu_antes_us = -1;
};
DiagAnillo g_diag_anillo;

// Used only by the server thread: it is the only one that reaches the hooked wait.
struct Rescate {
  bool activo = false;
  int64_t primera_espera_ms = -1;
  int64_t desde_ms = 0;
  int64_t ultimo_resumen_ms = 0;
  uint64_t fines_al_entrar = 0;
  uint64_t liberados = 0;
  uint64_t reentregas = 0;
  uint64_t veces = 0;
};
Rescate g_rescate;

void Volcar(const char* que, const uint8_t* base, uint32_t obj, int64_t ahora) {
  const uint32_t voz = Leer32(base, obj + kOffVoz);
  REXLOG_WARN("[audio] {}: R={} W={} estados={}/{} activo={} voz=0x{:08X} estado_voz=0x{:02X} "
              "fines={} pasadas={} entregas={} ms_sin_fin={} ms_sin_entrega={}",
              que, Leer32(base, obj + kOffR), Leer32(base, obj + kOffW), Estado(base, obj, 0),
              Estado(base, obj, 1), static_cast<unsigned>(base[obj + kOffActivo]), voz,
              static_cast<unsigned>(voz ? base[voz + kOffEstadoVoz] : 0), g_fines.load(),
              g_pasadas.load(), g_entregas.load(), ahora - g_ultimo_fin_ms.load(),
              ahora - g_ultima_entrega_ms.load());
}

// nfsmw_audio_diag_anillo: packets submitted to the voice (state 2) on each pass, with a
// summary every 10 s.
void AnotarPasada(const uint8_t* base) {
  const uint32_t obj = g_obj_servidor.load(std::memory_order_relaxed);
  if (obj == 0) {
    return;
  }
  DiagAnillo& d = g_diag_anillo;
  const uint32_t entregados = uint32_t(Estado(base, obj, 0) == kEntregado) +
                              uint32_t(Estado(base, obj, 1) == kEntregado);
  ++d.pasadas_con[entregados];
  const int64_t ahora = g_reloj_ms();
  const uint64_t fines = g_fines.load(std::memory_order_relaxed);
  const uint64_t entregas = g_entregas.load(std::memory_order_relaxed);
  const int64_t ultima_entrega = g_ultima_entrega_ms.load(std::memory_order_relaxed);
  if (ultima_entrega != 0) {
    d.max_sin_entrega_ms = std::max(d.max_sin_entrega_ms, ahora - ultima_entrega);
  }
  if (d.desde_ms == 0) {
    d.desde_ms = ahora;
    d.fines_antes = fines;
    d.entregas_antes = entregas;
    d.cpu_antes_us = nfsmw::hilos::CpuHiloUs(g_manejador_servidor.load(std::memory_order_relaxed));
    return;
  }
  if (ahora - d.desde_ms >= std::max<int64_t>(100, REXCVAR_GET(nfsmw_audio_diag_anillo_ms))) {
    const uint64_t mezclas = g_mezclas.exchange(0, std::memory_order_relaxed);
    const uint64_t mezcla_suma_us = g_mezcla_suma_us.exchange(0, std::memory_order_relaxed);
    const int64_t mezcla_max_us = g_mezcla_max_us.exchange(0, std::memory_order_relaxed);
    const uint64_t esperas = g_esperas.exchange(0, std::memory_order_relaxed);
    const uint64_t esperas_agotadas = g_esperas_agotadas.exchange(0, std::memory_order_relaxed);
    const uint64_t espera_suma_us = g_espera_suma_us.exchange(0, std::memory_order_relaxed);
    const int64_t espera_max_us = g_espera_max_us.exchange(0, std::memory_order_relaxed);
    const int64_t cpu_us = nfsmw::hilos::CpuHiloUs(g_manejador_servidor.load(std::memory_order_relaxed));
    const int64_t cpu_ventana_us = cpu_us >= 0 && d.cpu_antes_us >= 0 ? cpu_us - d.cpu_antes_us : -1;
    const int64_t ventana_ms = std::max<int64_t>(1, ahora - d.desde_ms);
    uint64_t nucleos[5] = {0, 0, 0, 0, 0};
    for (size_t i = 0; i < 5; ++i) {
      nucleos[i] = g_despertares_nucleo[i].exchange(0, std::memory_order_relaxed);
    }
    REXLOG_INFO("[audio] anillo del servidor en {} ms: pasadas con 0/1/2 paquetes en la voz "
                "{}/{}/{}, fines de paquete {}, entregas {}, maximo sin entrega {} ms, de despertar a "
                "entregar media {} us y maximo {} us ({} veces), esperas antes de la trama {} (agotadas {}), "
                "espera media {} us y maxima {} us, CPU del hilo servidor {} ms ({} %), despertares por nucleo "
                "0/1/2/3/otros {}/{}/{}/{}/{}",
                ahora - d.desde_ms, d.pasadas_con[0], d.pasadas_con[1], d.pasadas_con[2],
                fines - d.fines_antes, entregas - d.entregas_antes, d.max_sin_entrega_ms,
                mezclas ? mezcla_suma_us / mezclas : 0, mezcla_max_us, mezclas, esperas, esperas_agotadas,
                esperas ? espera_suma_us / esperas : 0, espera_max_us,
                cpu_ventana_us >= 0 ? cpu_ventana_us / 1000 : -1,
                cpu_ventana_us >= 0 ? cpu_ventana_us / (ventana_ms * 10) : -1, nucleos[0], nucleos[1],
                nucleos[2], nucleos[3], nucleos[4]);
    d = DiagAnillo{};
    d.desde_ms = ahora;
    d.fines_antes = fines;
    d.entregas_antes = entregas;
    d.cpu_antes_us = cpu_us;
  }
}

// nfsmw_audio_diag_retraso_servidor_us: busy-wait, so that the delay is the requested one even
// below a millisecond on Windows.
void RetrasarDespertar(int32_t us) {
  const auto fin = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
  while (std::chrono::steady_clock::now() < fin) {
    std::this_thread::yield();
  }
}

}  // namespace
}  // namespace nfsmw::audio_servidor

namespace nfsmw::audio_servidor {
namespace {

// SDK hook before each audio frame (rex::audio::SetGanchoAntesDeTrama), on the Audio Worker thread. Waits on
// the condition with 1 ms timeouts: the submission notifies before the game marks the packet as submitted, so
// each notification is checked again and a lost notification only costs that millisecond. It never spins: on
// the Switch the Audio Worker has a higher priority than the server itself.
void EsperarPaqueteServidor(size_t) {
  const int32_t maximo_ms = REXCVAR_GET(nfsmw_audio_esperar_servidor_ms);
  if (maximo_ms <= 0) {
    return;
  }
  const uint32_t obj = g_obj_servidor.load(std::memory_order_relaxed);
  const uint8_t* const base = g_base_servidor.load(std::memory_order_relaxed);
  if (obj == 0 || base == nullptr) {
    return;
  }
  const auto con_paquete = [&] {
    return Estado(base, obj, 0) == kEntregado || Estado(base, obj, 1) == kEntregado;
  };
  if (con_paquete()) {
    return;
  }
  if (base[kBanderaServidor] == 0 ||
      g_reloj_ms() - g_ultima_entrega_ms.load(std::memory_order_relaxed) > kServidorParadoMs) {
    return;
  }
  const int64_t inicio_us = RelojUs();
  const int64_t limite_us = inicio_us + int64_t(maximo_ms) * 1000;
  bool agotada = false;
  {
    std::unique_lock<std::mutex> cerrojo(g_entrega_mutex);
    while (!con_paquete()) {
      const int64_t ahora_us = RelojUs();
      if (ahora_us >= limite_us) {
        agotada = true;
        break;
      }
      g_entrega_cv.wait_for(cerrojo, std::chrono::microseconds(std::min<int64_t>(1000, limite_us - ahora_us)));
    }
  }
  const int64_t espera_us = RelojUs() - inicio_us;
  g_esperas.fetch_add(1, std::memory_order_relaxed);
  if (agotada) {
    g_esperas_agotadas.fetch_add(1, std::memory_order_relaxed);
  }
  g_espera_suma_us.fetch_add(static_cast<uint64_t>(espera_us), std::memory_order_relaxed);
  int64_t maximo = g_espera_max_us.load(std::memory_order_relaxed);
  while (espera_us > maximo &&
         !g_espera_max_us.compare_exchange_weak(maximo, espera_us, std::memory_order_relaxed)) {
  }
}

}  // namespace
}  // namespace nfsmw::audio_servidor

REX_EXTERN(__imp__sub_828531E8);
REX_EXTERN(__imp__sub_825E4358);
REX_EXTERN(__imp__sub_825E4338);
REX_EXTERN(__imp__sub_82851F00);
REX_EXTERN(__imp__sub_82851FB8);
REX_EXTERN(sub_82851D98);

namespace nfsmw::audio_servidor {
namespace {

// Submits the full packets in order while the voice has a free node, with
// the same condition and the same steps as 0x825E4030-0x825E4090.
uint64_t Reentregar(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  uint64_t entregados = 0;
  for (int vuelta = 0; vuelta < 2; ++vuelta) {
    const uint32_t r = Leer32(base, obj + kOffR) % 2;
    if (Estado(base, obj, r) != kLleno) {
      break;
    }
    const uint32_t salida = ctx.r1.u32 + 80;
    ctx.r3.u64 = Leer32(base, obj + kOffVoz);
    ctx.r4.u64 = salida;
    sub_82851D98(ctx, base);
    if ((base[salida] & kBitNodoLibre) == 0) {
      break;
    }
    ctx.r3.u64 = Leer32(base, obj + kOffVoz);
    ctx.r4.u64 = obj + kOffPaquetes + kBytesPaquete * r;
    ctx.r5.u64 = 0;
    __imp__sub_82851F00(ctx, base);
    Escribir32(base, obj + kOffEstados + 4 * r, kEntregado);
    Escribir32(base, obj + kOffR, (r + 1) % 2);
    ++entregados;
  }
  return entregados;
}

}  // namespace
}  // namespace nfsmw::audio_servidor

REX_HOOK_RAW(sub_825E4358) {
  using namespace nfsmw::audio_servidor;
  g_fines.fetch_add(1, std::memory_order_relaxed);
  g_ultimo_fin_ms.store(g_reloj_ms(), std::memory_order_relaxed);
  __imp__sub_825E4358(ctx, base);
}

REX_HOOK_RAW(sub_825E4338) {
  using namespace nfsmw::audio_servidor;
  g_pasadas.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(nfsmw_audio_diag_anillo)) {
    AnotarPasada(base);
  }
  __imp__sub_825E4338(ctx, base);
}

REX_HOOK_RAW(sub_82851F00) {
  using namespace nfsmw::audio_servidor;
  const bool del_servidor = static_cast<uint32_t>(ctx.lr) == kRetornoEntrega;
  const bool diag = REXCVAR_GET(nfsmw_audio_diag_anillo);
  const double lentitud = REXCVAR_GET(nfsmw_audio_diag_lentitud_mezcla);
  // Submission time taken before the call: it counts the mix, not how long XAudio takes to accept the packet.
  // Only the first submission after each wakeup.
  const int64_t entrega_us = del_servidor && (diag || lentitud > 0.0) ? RelojUs() : 0;
  const int64_t despertar_us = entrega_us != 0 ? g_despertar_us.exchange(0, std::memory_order_relaxed) : 0;
  int64_t mezcla_us = despertar_us != 0 ? entrega_us - despertar_us : -1;
  if (mezcla_us > 0 && lentitud > 0.0) {
    // nfsmw_audio_diag_lentitud_mezcla: the mix takes (1 + lentitud) times the measured time; the logged value is
    // the simulated one.
    RetrasarDespertar(static_cast<int32_t>(std::min(double(mezcla_us) * lentitud, 50000.0)));
    mezcla_us = RelojUs() - despertar_us;
  }
  __imp__sub_82851F00(ctx, base);
  if (del_servidor) {
    g_entregas.fetch_add(1, std::memory_order_relaxed);
    g_ultima_entrega_ms.store(g_reloj_ms(), std::memory_order_relaxed);
    if (REXCVAR_GET(nfsmw_audio_esperar_servidor_ms) > 0) {
      {
        std::lock_guard<std::mutex> cerrojo(g_entrega_mutex);
      }
      g_entrega_cv.notify_all();
    }
    if (mezcla_us >= 0 && diag) {
      const int64_t us = mezcla_us;
      g_mezclas.fetch_add(1, std::memory_order_relaxed);
      g_mezcla_suma_us.fetch_add(static_cast<uint64_t>(us), std::memory_order_relaxed);
      int64_t maximo = g_mezcla_max_us.load(std::memory_order_relaxed);
      while (us > maximo && !g_mezcla_max_us.compare_exchange_weak(maximo, us, std::memory_order_relaxed)) {
      }
    }
  }
}

REX_HOOK_RAW(sub_82851FB8) {
  using namespace nfsmw::audio_servidor;
  const bool del_servidor = static_cast<uint32_t>(ctx.lr) == kRetornoArranque;
  __imp__sub_82851FB8(ctx, base);
  if (del_servidor) {
    REXLOG_INFO("[audio] voz del servidor de audio arrancada: resultado 0x{:08X}", ctx.r3.u32);
  }
}

namespace nfsmw::audio_perfil {
void MarcarHiloServidor();  // nfsmw_audio_perfil_funciones.cpp (nfsmw_audio_diag_funciones)
}  // namespace nfsmw::audio_perfil

REX_HOOK_RAW(sub_828531E8) {
  using namespace nfsmw::audio_servidor;
  const uint32_t obj = ctx.r31.u32;
  if (static_cast<uint32_t>(ctx.lr) != kRetornoEspera || !REXCVAR_GET(nfsmw_audio_rescate) ||
      obj == 0 || Leer32(base, obj + kOffEvento) != ctx.r3.u32) {
    __imp__sub_828531E8(ctx, base);
    return;
  }
  const uint32_t evento = ctx.r3.u32;
  const uint32_t alertable = ctx.r5.u32;
  Rescate& r = g_rescate;
  g_obj_servidor.store(obj, std::memory_order_relaxed);
  g_base_servidor.store(base, std::memory_order_relaxed);
  // The SDK hook is registered once, from the server thread itself, once its object and the base are known.
  static std::atomic<bool> gancho_registrado{false};
  if (!gancho_registrado.exchange(true, std::memory_order_relaxed)) {
    rex::audio::SetGanchoAntesDeTrama(&EsperarPaqueteServidor);
    REXLOG_INFO("[audio] espera al servidor antes de cada trama: como mucho {} ms (0 = apagada)",
                REXCVAR_GET(nfsmw_audio_esperar_servidor_ms));
  }
  // Once per thread, from the server thread itself: its handle (for the CPU in the packet ring summaries), the
  // server thread mark for nfsmw_audio_diag_funciones, and nfsmw_audio_servidor_prioridad.
  thread_local bool prioridad_aplicada = false;
  if (!prioridad_aplicada) {
    prioridad_aplicada = true;
    g_manejador_servidor.store(nfsmw::hilos::ManejadorHiloActual(), std::memory_order_relaxed);
    nfsmw::audio_perfil::MarcarHiloServidor();
    const int32_t prioridad = REXCVAR_GET(nfsmw_audio_servidor_prioridad);
    if (prioridad >= 0x1C && prioridad <= 0x3A) {
      const bool hecho = nfsmw::hilos::PrioridadHiloActual(prioridad);
      REXLOG_INFO("[audio] hilo servidor de audio a prioridad 0x{:X}: {}", prioridad,
                  hecho ? "hecho" : "no disponible en esta plataforma");
    } else if (prioridad != 0) {
      REXLOG_WARN("[audio] nfsmw_audio_servidor_prioridad = {} fuera de 0x1C-0x3A: se ignora", prioridad);
    }
  }
  for (;;) {
    if (r.primera_espera_ms < 0) {
      r.primera_espera_ms = g_reloj_ms();
    }
    ctx.r3.u64 = evento;
    ctx.r4.u64 = static_cast<uint32_t>(r.activo ? kPlazoRescateMs : kPlazoMs);
    ctx.r5.u64 = alertable;
    __imp__sub_828531E8(ctx, base);
    const bool senal = ctx.r3.u32 != kStatusTimeout;
    // Diagnostic: simulates on PC the delay with which the thread runs again on the Switch.
    const int32_t retraso_us = REXCVAR_GET(nfsmw_audio_diag_retraso_servidor_us);
    if (senal && retraso_us > 0) {
      RetrasarDespertar(retraso_us);
    }
    if (senal && (REXCVAR_GET(nfsmw_audio_diag_anillo) || REXCVAR_GET(nfsmw_audio_diag_lentitud_mezcla) > 0.0)) {
      g_despertar_us.store(RelojUs(), std::memory_order_relaxed);
    }
    if (senal && REXCVAR_GET(nfsmw_audio_diag_anillo)) {
      const int nucleo = nfsmw::hilos::NucleoActual();
      g_despertares_nucleo[nucleo >= 0 && nucleo < 4 ? nucleo : 4].fetch_add(1, std::memory_order_relaxed);
    }
    const int64_t ahora = g_reloj_ms();

    if (r.activo && g_fines.load(std::memory_order_relaxed) != r.fines_al_entrar) {
      REXLOG_INFO("[audio] la voz vuelve a devolver fines de paquete tras {} ms de rescate "
                  "(paquetes liberados {}, reentregas {})",
                  ahora - r.desde_ms, r.liberados, r.reentregas);
      r.activo = false;
    }

    const bool w_ocupado = Estado(base, obj, Leer32(base, obj + kOffW) % 2) != kLibre;
    if (!r.activo) {
      const int64_t referencia =
          std::max({g_ultimo_fin_ms.load(std::memory_order_relaxed),
                    g_ultima_entrega_ms.load(std::memory_order_relaxed), r.primera_espera_ms});
      const bool atascado =
          w_ocupado && base[kBanderaServidor] != 0 && ahora - referencia >= kSinAvanceMs;
      if (!atascado) {
        if (senal) {
          break;
        }
        continue;
      }
      r.activo = true;
      r.desde_ms = ahora;
      r.ultimo_resumen_ms = ahora;
      r.fines_al_entrar = g_fines.load(std::memory_order_relaxed);
      if (++r.veces <= kMaxVolcados) {
        Volcar("el hilo servidor lleva 250 ms sin avanzar; entra en rescate", base, obj, ahora);
      }
    }

    // In rescue mode: first the same as the game would do, and if slot W is still
    // occupied it is freed so that the loop can mix the next packet.
    r.reentregas += Reentregar(ctx, base, obj);
    const uint32_t w = Leer32(base, obj + kOffW) % 2;
    if (Estado(base, obj, w) != kLibre) {
      Escribir32(base, obj + kOffEstados + 4 * w, kLibre);
      ++r.liberados;
    }
    if (ahora - r.ultimo_resumen_ms >= kResumenRescateMs && r.veces <= kMaxVolcados) {
      r.ultimo_resumen_ms = ahora;
      Volcar("sigue el rescate", base, obj, ahora);
    }
    break;
  }
  ctx.lr = kRetornoEspera;
}
