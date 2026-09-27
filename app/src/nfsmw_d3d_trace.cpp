// ===========================================================================
//  PHASE A OF THE NATIVE RENDERER: CONFIRM THE MAP OF THE GAME'S DIRECT3D
//
//  WHAT THIS IS
//  NFSMW carries its own Direct3D library from the Xbox 360 XDK, and the
//  native renderer replaces it, as MarathonRecomp-NX does. Before replacing
//  anything we had to be sure which function is which.
//
//  The map was obtained by reading the recompiled code: which function calls
//  VdSwap, which writes PM4 draw headers, which unpacks a color... That gives
//  candidates, not certainties. This file checks them at run time: it wraps
//  each function, always calls the original and records what happens.
//
//  With GPU emulation on, the game behaves the same as without this. The
//  only change is that sdmc:/switch/nfsmw/rex_d3d.log appears (or
//  rex_d3d.log next to the .exe on PC) with:
//
//    - how many times each function is called, and how many per frame
//    - the arguments of the first calls of each one
//    - where it is called from (the most frequent return addresses)
//    - a dump of the device object after it is created, which is where
//      the SetRenderState and SetSamplerState tables live
//    - the shader objects that get bound, to find where they are created
//
//  HOW THE HOOKING WORKS
//  Each recompiled function `sub_XXXXXXXX` is a weak alias of
//  `__imp__sub_XXXXXXXX`. Defining a strong `sub_XXXXXXXX` here wins, and all
//  calls (direct, and indirect through the function table) go through
//  here. The original is still available as `__imp__`, so wrapping means
//  calling it in the middle.
//
//  MIND THE COST
//  DrawIndexedVertices is called ~800 times per frame. That is why the
//  per-call work is two atomic counters. The report is written from Swap;
//  it is off by default so as not to add SD card writes to the game.
// ===========================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_nativo_ganchos.h"
#if defined(NFSMW_NATIVE_SHADER_LIBRARY)
#include "nfsmw_video_bridge.h"
#endif

REXCVAR_DEFINE_BOOL(nfsmw_nativo_gotas_lluvia, true, "NFSMW",
                    "Renderizador nativo (25/09, build 174): anotar los dibujos que el juego hace con la rutina "
                    "interna del D3D sub_825932D8 (las gotas de lluvia en la pantalla y el cuadrilatero del "
                    "VisualTreatment). Sin esto las gotas no salen. false = como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_d3d_trace, false, "NFSMW",
                    "Registrar las llamadas al Direct3D del juego en rex_d3d.log")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace nfsmw::d3d_trace {
namespace {

// The global pointer to the game's D3D device, found in the recompiled
// code: CreateDevice stores it there (lis r11,-32093 / addi -12448).
constexpr uint32_t kDeviceGlobal = 0x82A2CF60;

// How many words of the device are dumped. The SetRenderState table
// starts at +96 (0x61 entries) and the SetSamplerState table at +484; 0x400
// bytes are enough for both and for whatever lies in between.
constexpr uint32_t kDeviceDumpStart = 0x40;
constexpr uint32_t kDeviceDumpEnd = 0x400;

constexpr size_t kMaxDetail = 24;   // detailed calls per function
constexpr size_t kMaxCallers = 12;  // distinct callers per function
constexpr size_t kMaxShaders = 1024;  // objetos de shader recordados

/*
 * The driver separates the virtual part from the physical one: PS at object+0x34 and
 * [object+0x0C], VS at object+0x250 and [object+0x28]. Copying both contiguously
 * from the object produced 20 wrong dumps, which are kept for regression
 * testing. The originals were recovered from ZZDATA0.BIN and the XEX.
 * See docs/shaders.md: accepting a signature other than the 2008 one is not enough.
 */
constexpr uint32_t kContenedorFirma = 0x102A0E00;
constexpr uint32_t kContenedorMaximo = 64 * 1024;  // no real one comes close
// It stays off. New files go to a separate folder and do not overwrite
// the earlier ones. An object can contain metadata modified by the driver;
// to identify native shaders, the original from before its creation is used.
constexpr bool kVolcarShaders = false;
constexpr int kReportSeconds = 10;

enum Fn : uint32_t {
  kCreateDevice,
  kSwap,
  kSyncPresentInterval,
  kPresentComposite,
  kRingBufferParams,
  kClear,
  kClearF,
  kResolve,
  kBeginTiling,
  kEndTiling,
  kSetRenderTarget,
  kSetDepthStencilSurface,
  kCreateTexture,
  kCreateSurface,
  kSetTexture,
  kCreateVertexBuffer,
  kLockVertexBuffer,
  kCreateIndexBuffer,
  kLockIndexBuffer,
  kUnlock,
  kRelease,
  kSetStreamSource,
  kSetIndices,
  kCreateVertexDeclaration,
  kSetVertexDeclaration,
  kSetVertexShader,
  kSetPixelShader,
  kDrawVertices,
  kDrawIndexedVertices,
  kDrawVerticesUP,
  kDrawIndexedVerticesUP,
  kFlushState,
  kSetGammaRamp,
  kQueryBufferSpace,
  kFnCount,
};

struct FnInfo {
  const char* name;
  uint32_t address;
};

// The order must match the enum.
constexpr FnInfo kFns[kFnCount] = {
    {"CreateDevice", 0x825A1658},
    {"Swap", 0x825989D8},
    {"SyncToPresentationInterval", 0x82598868},
    {"PresentComposite", 0x82598FC8},
    {"SetRingBufferParameters", 0x825981E8},
    {"Clear", 0x8259A450},
    {"ClearF", 0x8259A500},
    {"Resolve", 0x82592538},
    {"BeginTiling", 0x825992F0},
    {"EndTiling", 0x82599680},
    {"SetRenderTarget", 0x8258DB18},
    {"SetDepthStencilSurface", 0x8258DE80},
    {"CreateTexture", 0x8258A0A0},
    {"CreateSurface", 0x8258A1C0},
    {"SetTexture", 0x8258A648},
    {"CreateVertexBuffer", 0x82595570},
    {"LockVertexBuffer", 0x82595620},
    {"CreateIndexBuffer", 0x825956D0},
    {"LockIndexBuffer", 0x82595780},
    {"Unlock", 0x82595170},
    {"Release", 0x825953D8},
    {"SetStreamSource", 0x8258D968},
    {"SetIndices", 0x8258DA60},
    {"CreateVertexDeclaration", 0x8259C470},
    {"SetVertexDeclaration", 0x8259C3D0},
    {"SetVertexShader", 0x8259C2A8},
    {"SetPixelShader", 0x8259BDC0},
    {"DrawVertices", 0x82593A10},
    {"DrawIndexedVertices", 0x82593C50},
    {"DrawVerticesUP", 0x82593588},
    {"DrawIndexedVerticesUP", 0x82593940},
    {"FlushState", 0x825A40C0},
    {"SetGammaRamp", 0x8258E138},
    {"QueryBufferSpace", 0x82597DA0},
};

struct Detail {
  uint64_t lr;
  uint32_t r[8];  // r3..r10
  double f1;
  uint32_t ret;  // r3 al volver
};

struct CallerCount {
  uint64_t lr;
  uint64_t n;
};

struct Slot {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> reported{0};  // calls already counted in a report
  std::atomic<uint32_t> detail_count{0};
  Detail detail[kMaxDetail]{};
  CallerCount callers[kMaxCallers]{};
};

Slot g_slots[kFnCount];
std::mutex g_mutex;  // only for the callers and the shaders

std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_started{false};

// Dump of the device, done once after CreateDevice.
std::atomic<bool> g_device_pending{false};
std::atomic<bool> g_device_written{false};
uint32_t g_device_addr = 0;
uint32_t g_device_words[(kDeviceDumpEnd - kDeviceDumpStart) / 4]{};

struct ShaderSeen {
  uint32_t object;
  uint32_t header[32];
  uint64_t lr;
  bool pixel;
  bool written;
  bool dumped;
};
ShaderSeen g_shaders[kMaxShaders]{};
size_t g_shader_count = 0;
size_t g_shaders_volcados = 0;      // contenedores escritos a la SD
size_t g_shaders_sin_contenedor = 0; // objects without a container inside

uint32_t LoadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t raw = 0;
  std::memcpy(&raw, base + address, sizeof(raw));
  return __builtin_bswap32(raw);
}

std::string ReportPath() {
  const auto folder = rex::filesystem::GetExecutableFolder();
  return (folder.empty() ? std::string("rex_d3d.log")
                         : (folder / "rex_d3d.log").string());
}

void WriteReport() {
  FILE* f = std::fopen(ReportPath().c_str(), "a");
  if (!f) {
    return;
  }

  const uint64_t frames = g_frames.load(std::memory_order_relaxed);
  std::fprintf(f, "==== fotogramas %llu\n", static_cast<unsigned long long>(frames));
  for (uint32_t i = 0; i < kFnCount; ++i) {
    const uint64_t calls = g_slots[i].calls.load(std::memory_order_relaxed);
    if (calls == 0) {
      continue;
    }
    std::fprintf(f, "  %-26s %10llu llamadas", kFns[i].name,
                 static_cast<unsigned long long>(calls));
    if (frames) {
      std::fprintf(f, "  (%.1f por fotograma)", double(calls) / double(frames));
    }
    std::fputc('\n', f);
  }

  // What follows is only written once per new piece of data.
  std::lock_guard<std::mutex> lock(g_mutex);

  if (g_device_pending.load(std::memory_order_acquire) &&
      !g_device_written.load(std::memory_order_relaxed)) {
    std::fprintf(f, "\n-- dispositivo en 0x%08X, palabras 0x%X..0x%X\n", g_device_addr,
                 kDeviceDumpStart, kDeviceDumpEnd);
    for (uint32_t off = 0; off < kDeviceDumpEnd - kDeviceDumpStart; off += 32) {
      std::fprintf(f, "   +0x%04X:", kDeviceDumpStart + off);
      for (uint32_t j = 0; j < 8 && off + j * 4 < kDeviceDumpEnd - kDeviceDumpStart; ++j) {
        std::fprintf(f, " %08X", g_device_words[(off / 4) + j]);
      }
      std::fputc('\n', f);
    }
    g_device_written.store(true, std::memory_order_relaxed);
  }

  for (uint32_t i = 0; i < kFnCount; ++i) {
    Slot& s = g_slots[i];
    const uint32_t n = s.detail_count.load(std::memory_order_acquire);
    bool header = false;
    for (uint32_t k = 0; k < n && k < kMaxDetail; ++k) {
      const Detail& d = s.detail[k];
      if (d.lr == 0) {
        continue;
      }
      if (!header) {
        std::fprintf(f, "\n-- %s (0x%08X): primeras llamadas\n", kFns[i].name, kFns[i].address);
        header = true;
      }
      std::fprintf(f,
                   "   lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X "
                   "r10=%08X f1=%.4f -> %08X\n",
                   static_cast<uint32_t>(d.lr), d.r[0], d.r[1], d.r[2], d.r[3], d.r[4], d.r[5],
                   d.r[6], d.r[7], d.f1, d.ret);
    }
    header = false;
    for (size_t k = 0; k < kMaxCallers; ++k) {
      if (s.callers[k].n == 0) {
        continue;
      }
      if (!header) {
        std::fprintf(f, "   llamantes: ");
        header = true;
      }
      std::fprintf(f, "%08X x%llu  ", static_cast<uint32_t>(s.callers[k].lr),
                   static_cast<unsigned long long>(s.callers[k].n));
    }
    if (header) {
      std::fputc('\n', f);
    }
  }
  std::fprintf(f, "\nshaders: %llu vistos, %llu volcados a shaders/, %llu sin contenedor\n",
               (unsigned long long)g_shader_count, (unsigned long long)g_shaders_volcados,
               (unsigned long long)g_shaders_sin_contenedor);

  for (size_t i = 0; i < g_shader_count; ++i) {
    ShaderSeen& sh = g_shaders[i];
    if (sh.written) {
      continue;
    }
    std::fprintf(f, "\n-- shader %s objeto 0x%08X (enlazado desde %08X)\n",
                 sh.pixel ? "de pixeles" : "de vertices", sh.object,
                 static_cast<uint32_t>(sh.lr));
    std::fprintf(f, "   cabecera:");
    for (uint32_t w : sh.header) {
      std::fprintf(f, " %08X", w);
    }
    std::fputc('\n', f);
    sh.written = true;
  }

  std::fputc('\n', f);
  std::fclose(f);
}

// No thread of its own. The first version launched one with std::thread and
// the game closed: detach() threw a std::system_error when called from a
// guest thread, inside the CreateDevice hook. And it is not needed: the report
// is written from the Swap hook, which already passes through here once per frame.
std::chrono::steady_clock::time_point g_last_report{};
std::atomic<bool> g_writing{false};

void MaybeReport() {
  const auto now = std::chrono::steady_clock::now();
  if (g_last_report.time_since_epoch().count() != 0 &&
      now - g_last_report < std::chrono::seconds(kReportSeconds)) {
    return;
  }
  bool expected = false;
  if (!g_writing.compare_exchange_strong(expected, true)) {
    return;  // another thread is writing: this frame skips it
  }
  g_last_report = now;
  WriteReport();
  g_writing.store(false);
}

// Trace startup, out of line and cold. It is the usual StartOnce body, unchanged: only StartOnce (right
// below) calls it, while g_started is still false.
[[gnu::noinline, gnu::cold]] void StartOnceLento() {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) {
    return;
  }
  g_enabled.store(REXCVAR_GET(nfsmw_d3d_trace), std::memory_order_relaxed);
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  if (FILE* f = std::fopen(ReportPath().c_str(), "w")) {
    std::fprintf(f, "Trazas del Direct3D de NFSMW\n\n");
    std::fclose(f);
  }
  REXLOG_INFO("Trazas de D3D activas: {}", ReportPath());
}

// Every hook in this file calls StartOnce on every call: in a race about 11,000-13,000 times per frame
// (SetStreamSource, FlushState, the Draw*, SetIndices, SetVertexDeclaration, the Set*Shader...; from a trace
// and the ELF). It used to be a compare_exchange_strong on every call: on the A57 (no LSE) an ldaxrb, an
// exclusive load with acquire, and since its cold path (fopen, ReportPath, REXLOG_INFO) was inlined, several
// hooks reserved a frame of 0x220-0x260 bytes and saved extra registers on every call. Now the usual path is
// a plain read of the flag, and the compare_exchange (the same one) is only done while it is still false, at
// startup. Same effect: startup happens only once, in the first hook to arrive, and nobody else writes
// g_started; a thread that sees g_started as true reads g_enabled just as before (the failed
// compare_exchange did not synchronize with the relaxed store of g_enabled that the winner does afterwards
// either).
[[gnu::always_inline]] inline void StartOnce() {
  if (g_started.load(std::memory_order_relaxed)) [[likely]] {
    return;
  }
  StartOnceLento();
}

void RecordCaller(Slot& s, uint64_t lr) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (size_t i = 0; i < kMaxCallers; ++i) {
    if (s.callers[i].n == 0) {
      s.callers[i].lr = lr;
      s.callers[i].n = 1;
      return;
    }
    if (s.callers[i].lr == lr) {
      ++s.callers[i].n;
      return;
    }
  }
}

// Template: that way there is no need to know which namespace PPCContext lives in.
// vegetacion = the kVeg* flags from DecidirVegetacion for the Draw* record.
template <typename Ctx>
void NotificarVideo(uint32_t fn, const Ctx& ctx, const uint8_t* base, uint16_t vegetacion = 0) {
  // Native renderer (part C5b): each Draw* leaves its VS and PS in the queue
  // of the PM4 ring sink. When off, it costs one atomic read.
  if (fn == kDrawVertices || fn == kDrawIndexedVertices || fn == kDrawVerticesUP ||
      fn == kDrawIndexedVerticesUP) {
    using nfsmw::nativo::FuncionDibujo;
    const FuncionDibujo funcion = fn == kDrawVertices          ? FuncionDibujo::kVertices
                                  : fn == kDrawIndexedVertices ? FuncionDibujo::kIndexados
                                  : fn == kDrawVerticesUP      ? FuncionDibujo::kVerticesUP
                                                               : FuncionDibujo::kIndexadosUP;
    nfsmw::nativo::AnotarDibujo(funcion, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                ctx.r7.u32, vegetacion);
  }
#if defined(NFSMW_NATIVE_SHADER_LIBRARY)
  if (fn == kDrawVertices || fn == kDrawIndexedVertices || fn == kDrawVerticesUP || fn == kDrawIndexedVerticesUP)
    nfsmw::native::AnotarDibujoVideo(base, fn == kDrawVertices && ctx.lr == 0x826DB87C, ctx.r31.u32);
  else if (fn == kClear || fn == kClearF) nfsmw::native::InvalidarVideo();
#else
  (void)fn; (void)ctx; (void)base;
#endif
}

template <typename Ctx>
void Enter(uint32_t fn, const Ctx& ctx) {
  Slot& s = g_slots[fn];
  const uint64_t n = s.calls.fetch_add(1, std::memory_order_relaxed);
  if (n < kMaxDetail * 4) {  // callers only at the beginning: this has to stay light
    RecordCaller(s, ctx.lr);
  }
  const uint32_t d = s.detail_count.load(std::memory_order_relaxed);
  if (d < kMaxDetail) {
    Detail& det = s.detail[d];
    det.lr = ctx.lr;
    det.r[0] = ctx.r3.u32;
    det.r[1] = ctx.r4.u32;
    det.r[2] = ctx.r5.u32;
    det.r[3] = ctx.r6.u32;
    det.r[4] = ctx.r7.u32;
    det.r[5] = ctx.r8.u32;
    det.r[6] = ctx.r9.u32;
    det.r[7] = ctx.r10.u32;
    det.f1 = ctx.f1.f64;
  }
}

template <typename Ctx>
void Leave(uint32_t fn, const Ctx& ctx) {
  Slot& s = g_slots[fn];
  const uint32_t d = s.detail_count.load(std::memory_order_relaxed);
  if (d < kMaxDetail) {
    s.detail[d].ret = ctx.r3.u32;
    s.detail_count.store(d + 1, std::memory_order_release);
  }
}

// Joins the two regions of the object without changing their byte order.
void VolcarShader(ShaderSeen& sh, const uint8_t* base) {
  if (!kVolcarShaders || sh.dumped) {
    return;
  }
  sh.dumped = true;

  const uint64_t contenedor = uint64_t(sh.object) + (sh.pixel ? 0x34 : 0x250);
  if (contenedor + 24 > (uint64_t(1) << 32)) return;
  const uint32_t firma = LoadGuestU32(base, uint32_t(contenedor));
  if (firma != (kContenedorFirma | (sh.pixel ? 0u : 1u))) {
    ++g_shaders_sin_contenedor;
    return;  // it has no container inside: nothing to dump
  }
  const uint32_t virtual_size = LoadGuestU32(base, uint32_t(contenedor + 4));
  const uint32_t physical_size = LoadGuestU32(base, uint32_t(contenedor + 8));
  const uint32_t fisica = LoadGuestU32(base, sh.object + (sh.pixel ? 0x0C : 0x28));
  const uint64_t total = uint64_t(virtual_size) + physical_size;
  if (virtual_size < 24 || !physical_size || physical_size % 12 || !fisica ||
      total > kContenedorMaximo || contenedor + virtual_size > (uint64_t(1) << 32) ||
      uint64_t(fisica) + physical_size > (uint64_t(1) << 32)) {
    return;
  }

  const auto folder = rex::filesystem::GetExecutableFolder();
  if (folder.empty()) {
    return;
  }
  const auto dir = folder / "shaders_2005_separados";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) return;

  char nombre[32];
  std::snprintf(nombre, sizeof(nombre), "%c_%08X.bin", sh.pixel ? 'p' : 'v', sh.object);
  const auto destino = dir / nombre;
  if (std::filesystem::exists(destino, ec) || ec) return;
  FILE* f = std::fopen(destino.string().c_str(), "wb");
  if (!f) {
    return;
  }
  const bool ok = std::fwrite(base + contenedor, 1, virtual_size, f) == virtual_size &&
                  std::fwrite(base + fisica, 1, physical_size, f) == physical_size;
  const int cierre = std::fclose(f);
  if (ok && !cierre) ++g_shaders_volcados;
}

void RememberShader(bool pixel, uint32_t object, uint64_t lr, const uint8_t* base) {
  if (object == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  for (size_t i = 0; i < g_shader_count; ++i) {
    if (g_shaders[i].object == object) {
      return;
    }
  }
  if (g_shader_count >= kMaxShaders) {
    return;
  }
  ShaderSeen& sh = g_shaders[g_shader_count++];
  sh.object = object;
  sh.lr = lr;
  sh.pixel = pixel;
  sh.written = false;
  sh.dumped = false;
  for (uint32_t i = 0; i < 32; ++i) {
    sh.header[i] = LoadGuestU32(base, object + i * 4);
  }
  VolcarShader(sh, base);
}

void CaptureDevice(const uint8_t* base) {
  if (g_device_pending.load(std::memory_order_acquire)) {
    return;
  }
  const uint32_t dev = LoadGuestU32(base, kDeviceGlobal);
  if (dev == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_device_addr = dev;
  for (uint32_t off = kDeviceDumpStart; off < kDeviceDumpEnd; off += 4) {
    g_device_words[(off - kDeviceDumpStart) / 4] = LoadGuestU32(base, dev + off);
  }
  g_device_pending.store(true, std::memory_order_release);
}

}  // namespace
}  // namespace nfsmw::d3d_trace

// ---------------------------------------------------------------------------
//  The hooks. One per function: enter, call the original, exit.
// ---------------------------------------------------------------------------
#define NFSMW_TRACE(addr, id)                                          \
  REX_EXTERN(__imp__sub_##addr);                                       \
  REX_HOOK_RAW(sub_##addr) {                                           \
    using namespace nfsmw::d3d_trace;                                  \
    StartOnce();                                                       \
    const bool on = g_enabled.load(std::memory_order_relaxed);         \
    if (on) {                                                          \
      Enter(id, ctx);                                                  \
    }                                                                  \
    NotificarVideo(id, ctx, base);                                     \
    __imp__sub_##addr(ctx, base);                                      \
    if (on) {                                                          \
      Leave(id, ctx);                                                  \
    }                                                                  \
  }

NFSMW_TRACE(82598868, nfsmw::d3d_trace::kSyncPresentInterval)
NFSMW_TRACE(82598FC8, nfsmw::d3d_trace::kPresentComposite)
NFSMW_TRACE(825981E8, nfsmw::d3d_trace::kRingBufferParams)
NFSMW_TRACE(8259A450, nfsmw::d3d_trace::kClear)
NFSMW_TRACE(8259A500, nfsmw::d3d_trace::kClearF)
NFSMW_TRACE(82592538, nfsmw::d3d_trace::kResolve)
NFSMW_TRACE(825992F0, nfsmw::d3d_trace::kBeginTiling)
NFSMW_TRACE(82599680, nfsmw::d3d_trace::kEndTiling)
NFSMW_TRACE(8258DB18, nfsmw::d3d_trace::kSetRenderTarget)
NFSMW_TRACE(8258DE80, nfsmw::d3d_trace::kSetDepthStencilSurface)
NFSMW_TRACE(8258A0A0, nfsmw::d3d_trace::kCreateTexture)
NFSMW_TRACE(8258A1C0, nfsmw::d3d_trace::kCreateSurface)
NFSMW_TRACE(8258A648, nfsmw::d3d_trace::kSetTexture)
NFSMW_TRACE(82595570, nfsmw::d3d_trace::kCreateVertexBuffer)
NFSMW_TRACE(82595620, nfsmw::d3d_trace::kLockVertexBuffer)
NFSMW_TRACE(825956D0, nfsmw::d3d_trace::kCreateIndexBuffer)
NFSMW_TRACE(82595780, nfsmw::d3d_trace::kLockIndexBuffer)
NFSMW_TRACE(82595170, nfsmw::d3d_trace::kUnlock)
NFSMW_TRACE(825953D8, nfsmw::d3d_trace::kRelease)
NFSMW_TRACE(8258D968, nfsmw::d3d_trace::kSetStreamSource)
NFSMW_TRACE(8258DA60, nfsmw::d3d_trace::kSetIndices)
NFSMW_TRACE(8259C470, nfsmw::d3d_trace::kCreateVertexDeclaration)
NFSMW_TRACE(8259C3D0, nfsmw::d3d_trace::kSetVertexDeclaration)
// Phase 2b of the Direct3D-level renderer: the four Draw* open the draw window before the original
// (AnotarDibujo, in NotificarVideo) and close it afterwards (TerminarDibujo). Their FlushState, which runs
// inside (all four always call it, as seen in the recompiled code), takes the record to the marker; if it
// did not, TerminarDibujo sends it to the usual queue. Otherwise, the same as NFSMW_TRACE.
template <typename Ctx, typename Original>
void GanchoDibujo(uint32_t id, Ctx& ctx, uint8_t* base, Original original) {
  using namespace nfsmw::d3d_trace;
  StartOnce();
  // nfsmw_d3d_vegetacion_juego: a DrawVertices or DrawIndexedVertices that the PM4 ring would discard as
  // shadow-map vegetation is skipped entirely here (DecidirVegetacion, nfsmw_nativo_ganchos.cpp); the others
  // carry the game's verdict in their record so that the ring can check it.
  bool saltar = false;
  const uint16_t vegetacion =
      (id == kDrawVertices || id == kDrawIndexedVertices)
          ? nfsmw::nativo::DecidirVegetacion(id == kDrawVertices ? nfsmw::nativo::FuncionDibujo::kVertices
                                                                 : nfsmw::nativo::FuncionDibujo::kIndexados,
                                             base, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, saltar)
          : uint16_t(0);
  if (saltar) {
    return;
  }
  const bool on = g_enabled.load(std::memory_order_relaxed);
  if (on) {
    Enter(id, ctx);
  }
  NotificarVideo(id, ctx, base, vegetacion);
  original(ctx, base);
  nfsmw::nativo::TerminarDibujo();
  if (on) {
    Leave(id, ctx);
  }
}
REX_EXTERN(__imp__sub_82593A10);
REX_HOOK_RAW(sub_82593A10) {  // DrawVertices
  GanchoDibujo(nfsmw::d3d_trace::kDrawVertices, ctx, base, [](auto& c, uint8_t* b) { __imp__sub_82593A10(c, b); });
}
REX_EXTERN(__imp__sub_82593C50);
REX_HOOK_RAW(sub_82593C50) {  // DrawIndexedVertices
  GanchoDibujo(nfsmw::d3d_trace::kDrawIndexedVertices, ctx, base,
               [](auto& c, uint8_t* b) { __imp__sub_82593C50(c, b); });
}
REX_EXTERN(__imp__sub_82593588);
REX_HOOK_RAW(sub_82593588) {  // DrawVerticesUP
  GanchoDibujo(nfsmw::d3d_trace::kDrawVerticesUP, ctx, base, [](auto& c, uint8_t* b) { __imp__sub_82593588(c, b); });
}
REX_EXTERN(__imp__sub_82593940);
REX_HOOK_RAW(sub_82593940) {  // DrawIndexedVerticesUP
  GanchoDibujo(nfsmw::d3d_trace::kDrawIndexedVerticesUP, ctx, base,
               [](auto& c, uint8_t* b) { __imp__sub_82593940(c, b); });
}
// FlushState goes through the composite marker first (phase 2 of the Direct3D-level renderer, at the end
// of nfsmw_d3d_registros_nativo.cpp). If that path declines it (off, or without the native renderer), the
// original runs. NotificarVideo does nothing with FlushState, so it is not called.
bool NfsmwFlushStateMarcador(PPCContext& ctx, uint8_t* base);
REX_EXTERN(__imp__sub_825A40C0);
REX_HOOK_RAW(sub_825A40C0) {  // FlushState
  using namespace nfsmw::d3d_trace;
  StartOnce();
  const bool on = g_enabled.load(std::memory_order_relaxed);
  if (on) {
    Enter(kFlushState, ctx);
  }
  if (!NfsmwFlushStateMarcador(ctx, base)) {
    __imp__sub_825A40C0(ctx, base);
  }
  if (on) {
    Leave(kFlushState, ctx);
  }
}
/*
 * Raindrops on the screen.
 *
 * On the Xbox 360, in the rain (the exit of the forest circuit), round drops that refract the image are
 * visible on the screen; here they did not show up. The game draws them in the final composition
 * (sub_82442478 -> sub_82448168 -> sub_8245C8D8, technique "onscreen_distort", PS p_000103 and VS
 * v_000098) by calling the D3D internal routine sub_825932D8 directly (it reserves space in the ring for a
 * draw with user vertices and returns where to copy them), without going through DrawVerticesUP. So no
 * Draw* record arrived and the PM4 ring had to deduce the shaders from what was loaded: the drops' VS is
 * not recognized (the D3D changes the swizzle of its fetches) and, without a VS, the draw was silently
 * discarded. The VisualTreatment quad (sub_82224458) uses the same shortcut; that one did show because its
 * VS is recognized.
 *
 * sub_825932D8(r3 device, r4 type, r5 vertex count, r6 stride): type and count are in the same registers
 * as in DrawVerticesUP, which is all the ring uses from a kVerticesUP record (EmparejarDibujo and
 * CuentaDelRegistro). The call DrawVerticesUP makes internally (return address 0x825935D8) is already
 * recorded: it is skipped.
 */
REX_EXTERN(__imp__sub_825932D8);
REX_HOOK_RAW(sub_825932D8) {
  static const bool gotas = REXCVAR_GET(nfsmw_nativo_gotas_lluvia);
  const bool anotado = gotas && uint32_t(ctx.lr) != 0x825935D8u;
  if (anotado) {
    nfsmw::nativo::AnotarDibujo(nfsmw::nativo::FuncionDibujo::kVerticesUP, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                                ctx.r6.u32, ctx.r7.u32);
  }
  __imp__sub_825932D8(ctx, base);
  if (anotado) {
    nfsmw::nativo::TerminarDibujo();  // phase 2b: if its FlushState did not take the record
  }
}

NFSMW_TRACE(8258E138, nfsmw::d3d_trace::kSetGammaRamp)
NFSMW_TRACE(82597DA0, nfsmw::d3d_trace::kQueryBufferSpace)

/*
 * IDirect3DQuery9::Issue (8258F810; r3 the query, r4 the flags: 2 = BEGIN, 1 = END). The vegetation
 * filtered in the game (nfsmw_d3d_vegetacion_juego) skips nothing while an occlusion query is open: there
 * the PM4 ring takes the long path. The original is always called and no PPC register is touched. Its
 * calls already go through sub_8258F810: the address appears in the sources, so tools/llamadas_directas.py
 * does not change them.
 */
REX_EXTERN(__imp__sub_8258F810);
REX_HOOK_RAW(sub_8258F810) {
  nfsmw::nativo::AnotarConsultaD3D(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_8258F810(ctx, base);
}

// The next four do more than count.

REX_EXTERN(__imp__sub_825A1658);
REX_HOOK_RAW(sub_825A1658) {  // CreateDevice: afterwards there is a device to look at
  using namespace nfsmw::d3d_trace;
  StartOnce();
  const bool on = g_enabled.load(std::memory_order_relaxed);
  if (on) {
    Enter(kCreateDevice, ctx);
  }
  __imp__sub_825A1658(ctx, base);
  if (on) {
    Leave(kCreateDevice, ctx);
    CaptureDevice(base);
    MaybeReport();  // the device dump, as soon as possible
  }
}

// Game frames, one per Swap, with or without tracing. Read by the
// F3 overlay FPS counter (MuestreaFotograma in nfsmw_app.h).
std::atomic<uint64_t> g_nfsmw_fotogramas_juego{0};

REX_EXTERN(__imp__sub_825989D8);
REX_HOOK_RAW(sub_825989D8) {  // Swap: marks the end of each frame
#if defined(NFSMW_NATIVE_SHADER_LIBRARY)
  nfsmw::native::AnotarSwapVideo();
#endif
  using namespace nfsmw::d3d_trace;
  StartOnce();
  const bool on = g_enabled.load(std::memory_order_relaxed);
  if (on) {
    Enter(kSwap, ctx);
    CaptureDevice(base);  // in case CreateDevice was called before this started
  }
  __imp__sub_825989D8(ctx, base);
  g_nfsmw_fotogramas_juego.fetch_add(1, std::memory_order_relaxed);
  if (on) {
    Leave(kSwap, ctx);
    g_frames.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }
}

REX_EXTERN(__imp__sub_8259C2A8);
REX_HOOK_RAW(sub_8259C2A8) {  // SetVertexShader
  using namespace nfsmw::d3d_trace;
  StartOnce();
  const bool on = g_enabled.load(std::memory_order_relaxed);
  const uint32_t shader = ctx.r4.u32;
  const uint64_t lr = ctx.lr;
  if (on) {
    Enter(kSetVertexShader, ctx);
  }
  __imp__sub_8259C2A8(ctx, base);
  if (on) {
    Leave(kSetVertexShader, ctx);
    RememberShader(false, shader, lr, base);
  }
}

REX_EXTERN(__imp__sub_8259BDC0);
REX_HOOK_RAW(sub_8259BDC0) {  // SetPixelShader
  using namespace nfsmw::d3d_trace;
  StartOnce();
  const bool on = g_enabled.load(std::memory_order_relaxed);
  const uint32_t shader = ctx.r4.u32;
  const uint64_t lr = ctx.lr;
  if (on) {
    Enter(kSetPixelShader, ctx);
  }
  __imp__sub_8259BDC0(ctx, base);
  if (on) {
    Leave(kSetPixelShader, ctx);
    RememberShader(true, shader, lr, base);
  }
}
