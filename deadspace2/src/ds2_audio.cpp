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
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif
#if defined(__aarch64__)
#include <arm_neon.h>
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
// system counter per call (a register read on the Switch, no system call), but some 200 thousand calls a second in a
// fight: off by default, for profiling runs. The stages nest (Dac::Process runs the Mixer, which runs the plug-ins),
// so they do not add up.

REXCVAR_DEFINE_BOOL(ds2_audio_tiempos, false, "DS2",
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
    {"MixWithGain"},           {"MixWithGainRamp"},       {"DecoderBaseModule::DecodeImpl"},
    {"EaLayer3DecBase::DecodeEvent"}, {"EaLayer3DecBase::DecodeGranule"}, {"Xas1Dec::DecodeEvent"},
    {"EaXmaDec::DecodeEvent"}, {"EaSpeexDec::DecodeEvent"}, {"Pcm16BigDec::DecodeEvent"},
    {"LinearInterpolate"},     {"ReChannelGainMixRamp"},  {"SndPlayer1::Declick"},
    {"Pan2D1::PanOutput"},     {"PolySynthBand"},         {"GainFader::Process"},
    {"Gain::Process"},
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
DS2_ETAPA(MixWithGainRamp, 25)
DS2_ETAPA(DecoderBaseModule__DecodeImpl, 26)
DS2_ETAPA(EaLayer3DecBase__DecodeEvent, 27)
DS2_ETAPA(EaLayer3DecBase__DecodeGranule, 28)
DS2_ETAPA(Xas1Dec__DecodeEvent, 29)
DS2_ETAPA(EaXmaDec__DecodeEvent, 30)
DS2_ETAPA(EaSpeexDec__DecodeEvent, 31)
DS2_ETAPA(Pcm16BigDec__DecodeEvent, 32)
DS2_ETAPA(LinearInterpolate, 33)
DS2_ETAPA(ReChannelGainMixRamp, 34)
DS2_ETAPA(SndPlayer1__Declick, 35)
DS2_ETAPA(Pan2D1__PanOutput, 36)
DS2_ETAPA(PolySynthBand, 37)
DS2_ETAPA(GainFader__Process, 38)
DS2_ETAPA(Gain__Process, 39)

// ---- Native versions of the mixer's leaf loops. Translated, each 4-float VMX vector goes through byte shuffles and
// stores in the context; MixWithGain alone runs 100-170 thousand times a second in a fight (about 14 % of a core at
// stock clocks). The same arithmetic on NEON: big-endian floats swapped on load and store, multiply then add (not
// fused), as vmaddfp.

REXCVAR_DEFINE_BOOL(ds2_audio_nativo, true, "DS2",
                    "MixWithGain y CopyWithGain del mezclador en NEON nativo en vez de traducidos");

namespace {

#if defined(__aarch64__)
inline float32x4_t CargarBe(const uint8_t* p) { return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p))); }
inline void GuardarBe(uint8_t* p, float32x4_t v) { vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(v))); }
#endif

inline float CargarFloatBe(const uint8_t* p) {
  uint32_t u;
  std::memcpy(&u, p, 4);
  u = __builtin_bswap32(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

inline void GuardarFloatBe(uint8_t* p, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(p, &u, 4);
}

// dst[i] = dst[i] + src[i] * gain (mezclar) or src[i] * gain.
void ConGanancia(uint8_t* dst, const uint8_t* src, float gain, uint32_t n, bool mezclar) {
  uint32_t i = 0;
#if defined(__aarch64__)
  const float32x4_t g = vdupq_n_f32(gain);
  for (; i + 8 <= n; i += 8) {
    const float32x4_t a = vmulq_f32(CargarBe(src + i * 4), g);
    const float32x4_t b = vmulq_f32(CargarBe(src + i * 4 + 16), g);
    if (mezclar) {
      GuardarBe(dst + i * 4, vaddq_f32(CargarBe(dst + i * 4), a));
      GuardarBe(dst + i * 4 + 16, vaddq_f32(CargarBe(dst + i * 4 + 16), b));
    } else {
      GuardarBe(dst + i * 4, a);
      GuardarBe(dst + i * 4 + 16, b);
    }
  }
#endif
  for (; i < n; ++i) {
    const float v = CargarFloatBe(src + i * 4) * gain;
    GuardarFloatBe(dst + i * 4, mezclar ? CargarFloatBe(dst + i * 4) + v : v);
  }
}

}  // namespace

// ---- The audio threads' priority (from ds3-nx, src/ds3_audio.cpp). The game's audio threads run at Horizon 0x3B, the
// band of its main thread and job workers; when those keep the cores busy (always at stock clocks, and more so once the
// GPU is no longer the limit) an audio thread gets its time slice too late and the frame goes out as silence. On the
// Xbox 360 they run time-critical. Each raises itself once to ds2_prioridad_mezclador, above the game's threads and
// below the ring thread (0x2C) and the XMA decoder (0x2B): RenderWare Audio's mixer threads (rw::audio::core::Dac::
// XenonThread, and the threads that call MixWithGain / CopyWithGain) and XAudio2's processing thread
// (LEAPCORE::CGraphManager::ThreadProc), which the game's audio callback waits on. They block on events between
// frames, so they cannot keep a core from the others. Dead Space 3 lost up to a third of its sound frames in fights at
// stock clocks before this.

REXCVAR_DEFINE_INT32(ds2_prioridad_mezclador, 0x2E, "DS2",
                     "Prioridad de Horizon de los hilos de mezcla de audio del juego (0x2C-0x3A); 0 = la de los "
                     "demas hilos del juego (0x3B)")
    .range(0, 0x3B);

#if defined(__SWITCH__)
extern "C" bool RexSwitchSetCurrentThreadPriorityOk(int priority);
#endif

namespace {
void ElevarHiloAudio(const char* que) {
  thread_local bool hecho = false;
  if (hecho) return;
  hecho = true;
#if defined(__SWITCH__)
  const int32_t prioridad = REXCVAR_GET(ds2_prioridad_mezclador);
  if (prioridad >= 0x2C && prioridad < 0x3B) {
    const bool ok = RexSwitchSetCurrentThreadPriorityOk(prioridad);
    REXLOG_INFO("[audio] {} a la prioridad 0x{:X}: {}", que, prioridad, ok ? "hecho" : "FALLO");
  }
#else
  (void)que;
#endif
}
}  // namespace

REX_EXTERN(__imp__rw__audio__core__Dac__XenonThread);
REX_EXTERN(rw__audio__core__Dac__XenonThread) {
  ElevarHiloAudio("hilo de mezcla de RenderWare Audio (Dac)");
  __imp__rw__audio__core__Dac__XenonThread(ctx, base);
}

REX_EXTERN(__imp__LEAPCORE__CGraphManager__ThreadProc);
REX_EXTERN(LEAPCORE__CGraphManager__ThreadProc) {
  ElevarHiloAudio("hilo de XAudio2 (CGraphManager)");
  __imp__LEAPCORE__CGraphManager__ThreadProc(ctx, base);
}

// void MixWithGain(float* dst, float* src, float gain, unsigned count)
REX_EXTERN(__imp__rw__audio__core__MixWithGain);
REX_EXTERN(rw__audio__core__MixWithGain) {
  ElevarHiloAudio("hilo de mezcla");
  const bool tiempos = REXCVAR_GET(ds2_audio_tiempos);
  const uint64_t inicio = tiempos ? Ticks() : 0;
  if (REXCVAR_GET(ds2_audio_nativo)) {
    ConGanancia(REX_RAW_ADDR(ctx.r3.u32), REX_RAW_ADDR(ctx.r4.u32), float(ctx.f1.f64), ctx.r6.u32, true);
  } else {
    __imp__rw__audio__core__MixWithGain(ctx, base);
  }
  if (tiempos) {
    g_etapas[24].ticks.fetch_add(Ticks() - inicio, std::memory_order_relaxed);
    g_etapas[24].llamadas.fetch_add(1, std::memory_order_relaxed);
  }
}

// void CopyWithGain(float* dst, float* src, float gain, unsigned count)
REX_EXTERN(__imp__rw__audio__core__CopyWithGain);
REX_EXTERN(rw__audio__core__CopyWithGain) {
  ElevarHiloAudio("hilo de mezcla");
  if (REXCVAR_GET(ds2_audio_nativo)) {
    ConGanancia(REX_RAW_ADDR(ctx.r3.u32), REX_RAW_ADDR(ctx.r4.u32), float(ctx.f1.f64), ctx.r6.u32, false);
    return;
  }
  __imp__rw__audio__core__CopyWithGain(ctx, base);
}
