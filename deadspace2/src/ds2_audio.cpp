// deadspace2 - lighter audio for the Switch's CPU.
//
// At stock clocks (CPU 1020 MHz) the game's own audio takes about 1.6 of the 3 cores the process has: the mixer thread
// (RwAudioCore Dac) alone sits at 85 %, and the game's main thread is starved. A third of the mixer's time was the
// master compressor (rw::audio::core::Compressor1::Process and the CompressorLimiter1 loop it calls): per-sample gain in
// dB through VMX log/exp estimates, translated to scalar log2f and float code.
//
// The compressor has its own bypass: with its threshold at 0 dB or its ratio at 1 it leaves the mixer's buffer as it
// is (it works in place), clears its state and returns 1. ds2_audio_sin_compresor takes that path always. What is lost
// is the dynamics compression of the final mix: loud moments are not squeezed (the output is still clamped).

#include "ds2_rex_diagnostic_pch.h"

#include <atomic>
#include <chrono>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(ds2_audio_sin_compresor, true, "DS2",
                    "Saltarse el compresor final del audio del juego (su propio camino de bypass): un tercio del hilo "
                    "de mezcla. false = el sonido original, mas CPU");

REX_EXTERN(__imp__rw__audio__core__Compressor1__Process);
REX_EXTERN(rw__audio__core__Compressor1__Process) {
  if (REXCVAR_GET(ds2_audio_sin_compresor)) {
    static bool avisado = false;
    if (!avisado) {
      avisado = true;
      REXLOG_INFO("[audio] compresor final del juego desactivado (ds2_audio_sin_compresor)");
    }
    ctx.r3.u64 = 1;  // what the bypass returns
    return;
  }
  __imp__rw__audio__core__Compressor1__Process(ctx, base);
}

// ---- Where the mixer's time goes: inclusive time of each stage of rwaudiocore, logged every 10 s. Two reads of the
// system counter per call (a register read on the Switch, no system call), so it stays on: the numbers decide what to
// lighten next. The stages nest (Dac::Process runs the Mixer, which runs the plug-ins), so they do not add up.

REXCVAR_DEFINE_BOOL(ds2_audio_tiempos, true, "DS2",
                    "Cada 10 s, el tiempo de CPU de cada etapa del audio del juego (rwaudiocore) en el log");

namespace {

uint64_t Ticks() {
#if defined(__SWITCH__)
  return armGetSystemTick();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

double TicksAMs(uint64_t ticks) {
#if defined(__SWITCH__)
  return double(armTicksToNs(ticks)) / 1e6;
#else
  return double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::duration(ticks)).count()) / 1e6;
#endif
}

struct Etapa {
  const char* nombre;
  std::atomic<uint64_t> llamadas{0};
  std::atomic<uint64_t> ticks{0};
};

Etapa g_etapas[] = {
    {"Dac::Process"},          {"Mixer::Execute"},       {"Mixer::ProcessInputPlugIns"},
    {"ReverbIR1::Process"},    {"ReverbModel1::Process"}, {"FastFirEngine::Filter"},
    {"ResampleHQ::Process"},   {"Resample::Process"},     {"Pan3D::Process"},
    {"Pan2D1::Process"},       {"MatrixPanner::Process"}, {"SndPlayer1::Process"},
    {"GenericPlayer::Process"}, {"PacketPlayer::Process"}, {"TimeStretch::Process"},
    {"Delay::Process"},        {"Limiter1::Process"},     {"CompressorLimiter1::Process"},
    {"Iir2::Filter"},          {"Fir64::Filter"},         {"SubMix::Process"},
    {"Send::Process"},         {"Dac::Mix"},              {"Dac::XenonDownMix"},
    {"MixWithGain"},           {"MixWithGainRamp"},
};

uint64_t g_ultimo_informe = 0;

void Informar(uint64_t ahora) {
  if (!g_ultimo_informe) {
    g_ultimo_informe = ahora;
    return;
  }
  const double ventana_ms = TicksAMs(ahora - g_ultimo_informe);
  if (ventana_ms < 10000.0) return;
  g_ultimo_informe = ahora;
  std::string linea;
  for (Etapa& e : g_etapas) {
    const uint64_t n = e.llamadas.exchange(0, std::memory_order_relaxed);
    const uint64_t t = e.ticks.exchange(0, std::memory_order_relaxed);
    if (!n) continue;
    linea += fmt::format(" | {} {:.1f}% ({:.0f}/s)", e.nombre, TicksAMs(t) * 100.0 / ventana_ms, n * 1000.0 / ventana_ms);
  }
  REXLOG_INFO("[audio] tiempo de CPU por etapa (inclusivo, % de un nucleo){}", linea);
}

struct Medida {
  Etapa& etapa;
  const uint64_t inicio = Ticks();
  explicit Medida(Etapa& e) : etapa(e) {}
  ~Medida() {
    etapa.ticks.fetch_add(Ticks() - inicio, std::memory_order_relaxed);
    etapa.llamadas.fetch_add(1, std::memory_order_relaxed);
  }
};

}  // namespace

#define DS2_ETAPA(funcion, indice)                       \
  REX_EXTERN(__imp__rw__audio__core__##funcion);         \
  REX_EXTERN(rw__audio__core__##funcion) {               \
    if (!REXCVAR_GET(ds2_audio_tiempos)) {               \
      __imp__rw__audio__core__##funcion(ctx, base);      \
      return;                                            \
    }                                                    \
    Medida medida(g_etapas[indice]);                     \
    __imp__rw__audio__core__##funcion(ctx, base);        \
  }

REX_EXTERN(__imp__rw__audio__core__Dac__Process);
REX_EXTERN(rw__audio__core__Dac__Process) {
  if (!REXCVAR_GET(ds2_audio_tiempos)) {
    __imp__rw__audio__core__Dac__Process(ctx, base);
    return;
  }
  {
    Medida medida(g_etapas[0]);
    __imp__rw__audio__core__Dac__Process(ctx, base);
  }
  Informar(Ticks());
}
DS2_ETAPA(Mixer__Execute, 1)
DS2_ETAPA(Mixer__ProcessInputPlugIns, 2)
DS2_ETAPA(ReverbIR1__Process, 3)
DS2_ETAPA(ReverbModel1__Process, 4)
DS2_ETAPA(FastFirEngine__Filter, 5)
DS2_ETAPA(ResampleHQ__Process, 6)
DS2_ETAPA(Resample__Process, 7)
DS2_ETAPA(Pan3D__Process, 8)
DS2_ETAPA(Pan2D1__Process, 9)
DS2_ETAPA(MatrixPanner__Process, 10)
DS2_ETAPA(SndPlayer1__Process, 11)
DS2_ETAPA(GenericPlayer__Process, 12)
DS2_ETAPA(PacketPlayer__Process, 13)
DS2_ETAPA(TimeStretch__Process, 14)
DS2_ETAPA(Delay__Process, 15)
DS2_ETAPA(Limiter1__Process, 16)
DS2_ETAPA(CompressorLimiter1__Process, 17)
DS2_ETAPA(Iir2__Filter, 18)
DS2_ETAPA(Fir64__Filter, 19)
DS2_ETAPA(SubMix__Process, 20)
DS2_ETAPA(Send__Process, 21)
DS2_ETAPA(Dac__Mix, 22)
DS2_ETAPA(Dac__XenonDownMix, 23)
DS2_ETAPA(MixWithGain, 24)
DS2_ETAPA(MixWithGainRamp, 25)
