// deadspace2 - the game's graphics system on the Switch.
//
// Copied from halo3/src/halo3_gpu.cpp: nothing in it is about Halo 3. Once a second game uses it, it belongs in sdk/.
//
// What it does
//   Stands in for the Xbox 360 GPU, so the game runs past its first frame. The game's own Direct3D keeps
//   running unchanged and writes its list of GPU commands (the PM4 ring). A thread reads that list and does
//   what the game waits for:
//     1. MMIO registers at 0x7FC80000, with the fixed values the SDK's emulation returns;
//     2. the ring read pointer, written back where the game watches it (it spins on it to know how much
//        room is left);
//     3. interrupts: vblank, and PM4_INTERRUPT packets (the GPU reached that point of the ring);
//     4. GPU writes to memory (MEM_WRITE, COND_WRITE, REG_TO_MEM, EVENT_WRITE_*, scratch registers), as in
//        the SDK's graphics/command_processor.cpp. Occlusion queries report 1000 samples, like the SDK.
//   5. (deko3d build) the minimal renderer: IM_LOAD / IM_LOAD_IMMEDIATE tell which translated shader is loaded
//      (ds2_shaders.h), and each draw goes to the SDK's deko3d backend (rex::x360::RenderDk) as a PeticionDibujo
//      with the register mirror; resolves and clears go to its Copiar, and each Swap presents the frontbuffer
//      that fetch constant 0 describes. A draw whose VS or PS is not identified is skipped and counted.
//
// Presentation (the deko3d build): the SDK's deko3d provider and presenter (rex/ui/deko3d/presentador.h). ReXApp
// sees them and sets up the window and the ImGui overlay as usual; until the renderer refreshes the guest output,
// the presenter paints black under the overlay.
//
// Without it the runtime drops the game's Vd* calls ("no GPU emulation loaded") and the render thread waits
// forever for a GPU that never reads its commands.

#include "ds2_gpu.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <vector>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/types.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

#if DS2_DEKO3D
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <string>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/ui/deko3d/presentador.h>
#include <rex/x360/decodificador.h>
#include <rex/x360/deko/destinos.h>
#include <rex/x360/destinos.h>
#include <rex/x360/peticion.h>

#include "ds2_shaders.h"

// xxHash reads with memcpy under GCC's strict aliasing (see shaders/ds_shader_library.cpp).
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DECLARE(bool, switch_captura_pedida);  // ZL + ZR + Down (sdk/src/input/switch/switch_input_driver.cpp)
REXCVAR_DECLARE(bool, switch_volcado_pedido);  // ZL + ZR + Left: every resolved texture of the next frame
REXCVAR_DECLARE(bool, switch_omitir_pedido);   // ZL + ZR + Right: next draw-skipping mode (SkipMode)
// The SDK renderer's shadow-pass shortcuts (sdk/src/x360/deko/destinos.cpp; the SDK keeps their nfsmw_ names): both
// drop colorless draws that alpha-test or discard, which here are ordinary depth-only draws. Off for Dead Space 2.
REXCVAR_DECLARE(bool, nfsmw_sombras_sin_vegetacion);
REXCVAR_DECLARE(bool, nfsmw_nativo_ps_solo_alfa);
// The renderer's shadow-map depth bias, given when the game sets none: Dead Space 2's shadow maps are 960 wide.
REXCVAR_DECLARE(int32_t, nfsmw_dk_sombras_sesgo_pitch);
// The renderer's sampled change checks of geometry and textures stay at the SDK's defaults (sampled). Full fingerprints
// were tried against the streaks (which were the vertex index and the computed-index fetches, not stale vertices):
// they hashed 14-20 MB of vertices a frame and the ring thread went from 9 % to 40 % of a core.
REXCVAR_DECLARE(bool, nfsmw_dk_geometria_muestreo);
REXCVAR_DECLARE(int32_t, nfsmw_dk_perfil_gpu_s);  // the renderer's periodic GPU profile (telemetry only)
// Safe mode (ZL + ZR + Right, mode 1): every shortcut the renderer took for NFS Most Wanted off at once, to tell whether
// one of them breaks Dead Space 2 (each changes what is drawn when its guess about the game is wrong). The SDK reads
// them on every use, so they switch while the game runs.
REXCVAR_DECLARE(bool, nfsmw_nativo_saltar_invisibles);
REXCVAR_DECLARE(bool, nfsmw_nativo_sin_ps_sin_color);
REXCVAR_DECLARE(bool, nfsmw_nativo_sin_desenfoque);
REXCVAR_DECLARE(bool, nfsmw_nativo_pcf_barato);
REXCVAR_DECLARE(bool, nfsmw_nativo_sombra_minimo);
REXCVAR_DECLARE(bool, nfsmw_nativo_z_temprana);
REXCVAR_DECLARE(bool, nfsmw_nativo_cielo_aplazado);
REXCVAR_DECLARE(bool, nfsmw_dk_constantes_por_diferencias);
REXCVAR_DECLARE(bool, nfsmw_nativo_saltar_borrados_repetidos);
REXCVAR_DECLARE(bool, nfsmw_nativo_resolver_sin_copia);
REXCVAR_DECLARE(bool, nfsmw_nativo_intercambiar_sin_borrado);
REXCVAR_DECLARE(bool, nfsmw_nativo_frontal_perezoso);
REXCVAR_DECLARE(bool, nfsmw_dk_borrar_area_util);
REXCVAR_DECLARE(bool, nfsmw_dk_borrado_color_aplazado);
REXCVAR_DECLARE(bool, nfsmw_dk_borrado_z_aplazado);

// Automatic data collection while the renderer is being brought up: nothing to press on the console.
REXCVAR_DEFINE_BOOL(ds2_oclusion_real, true, "DS2",
                    "Contar en la GPU las muestras de las consultas de oclusion del juego (sdk-patches/0017) en vez de "
                    "contestar siempre 'visible'. La pelea final mide con ellas cuanto de Nicole se ve");
REXCVAR_DEFINE_INT32(ds2_omitir_efectos, 0, "DS2",
                     "Diagnostico desde el menu (ZL + ZR + Menos): no dibujar un tipo de dibujo. 0 = nada, 1 luces, "
                     "2 aditivos, 3 con transparencia, 4 listas de cuadrados, 5 todo lo mezclado, 6 con prueba de alfa")
    .range(0, 6);
REXCVAR_DEFINE_BOOL(ds2_borrado_solo_salidas, true, "DS2",
                    "Borrados de Direct3D: borrar la profundidad por c1-c3 solo si el PS del borrado escribe en ellos "
                    "(como la Xbox 360; ds3-nx). false = como antes (todos los c1-c3 con mascara)");
REXCVAR_DEFINE_INT32(ds2_auto_captura_s, 5, "DS2",
                     "Cada cuantos segundos se guarda una captura reducida (640x360) en capturas/auto_NNNN.bmp; 0 = nunca");
REXCVAR_DEFINE_INT32(ds2_auto_volcado_primero_s, 120, "DS2",
                     "Segundo de juego del primer volcado automatico de las texturas resueltas de un fotograma");
REXCVAR_DEFINE_INT32(ds2_auto_volcado_cada_s, 300, "DS2", "Segundos entre volcados automaticos; 0 = solo el primero");
REXCVAR_DEFINE_INT32(ds2_auto_volcados_max, 4, "DS2", "Volcados automaticos como mucho (0 = ninguno)");
REXCVAR_DEFINE_INT32(ds2_traza_cada_s, 60, "DS2", "Segundos entre trazas de la estructura de un fotograma pesado");
REXCVAR_DEFINE_INT32(ds2_trazas_max, 20, "DS2", "Trazas de fotograma como mucho");
REXCVAR_DEFINE_BOOL(ds2_perfil_pilas, true, "DS2",
                    "Crea logs/rex/perfil_pilas.flag al arrancar: el perfil de CPU muestrea las pilas de cada hilo "
                    "(rex_perfil.log), algo mas lento");
#endif

namespace ds2::gpu {
namespace {

// The gamma ramp D3DDevice_SetGammaRamp left (ds2_cpu.cpp hooks it), waiting for the ring thread's next present.
std::mutex g_rampa_cerrojo;
std::array<std::array<uint16_t, 3>, 256> g_rampa{};
bool g_rampa_nueva = false;

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;  // X_STATUS_SUCCESS and X_STATUS_UNSUCCESSFUL need it too
using Clock = std::chrono::steady_clock;

constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegisterCount = 0x5003;  // RegisterFile::kRegisterCount

constexpr uint32_t kRegCpRbWptr = rex::graphics::XE_GPU_REG_CP_RB_WPTR;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;

constexpr int kMaxIndirectDepth = 4;
// The real GPU waits as long as it takes; without one some conditions are never met, so the wait is cut
// short and the ring goes on.
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint16_t kMaxExtent = 2048 >> 3;  // xenos::kTexture2DCubeMaxWidthHeight >> 3
constexpr int32_t kFakeSampleCount = 1000;  // the SDK's query_occlusion_fake_sample_count
constexpr auto kReportEvery = std::chrono::seconds(10);

// Reads the ring (a power-of-two buffer of words that wraps) or an indirect buffer (linear).
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t mask = 0;  // words - 1 in the ring; 0 in a linear buffer
  uint32_t pos = 0;
  uint32_t end = 0;

  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Peek() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t value = Peek();
    Skip(1);
    return value;
  }
  void Skip(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compare(uint32_t info, uint32_t value, uint32_t reference) {
  switch (info & 0x7) {
    case 0x0: return false;
    case 0x1: return value < reference;
    case 0x2: return value <= reference;
    case 0x3: return value == reference;
    case 0x4: return value != reference;
    case 0x5: return value >= reference;
    case 0x6: return value > reference;
    default: return true;
  }
}

// Register base of each SET_CONSTANT / LOAD_ALU_CONSTANT type, or UINT32_MAX.
uint32_t ConstantBase(uint32_t type_index) {
  switch ((type_index >> 16) & 0xFF) {
    case 0: return 0x4000;  // ALU
    case 1: return 0x4800;  // fetch
    case 2: return 0x4900;  // bool
    case 3: return 0x4908;  // loop
    case 4: return 0x2000;  // registers
    default: return UINT32_MAX;
  }
}

class RingGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  RingGraphicsSystem() : registers_(kRegisterCount, 0) {}
  ~RingGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
#if DS2_DEKO3D
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    if (!provider_) {
      provider_ = rex::deko3d::ProveedorDeko3d::Crear();  // a deko3d failure ends the process, logged
      if (!provider_) {
        REXLOG_ERROR("[gpu] could not create the deko3d device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[gpu] could not create the deko3d presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[gpu] deko3d presenter up (the game's frames come with the renderer)");
#else
    (void)app_context;
#endif
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ =
          memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this, &ReadMmio, &WriteMmio);
      if (!mmio_registered_) {
        REXLOG_ERROR("[gpu] could not register the GPU's MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) {
      return X_STATUS_SUCCESS;
    }
#if DS2_DEKO3D
    REXCVAR_SET(nfsmw_sombras_sin_vegetacion, false);
    REXCVAR_SET(nfsmw_nativo_ps_solo_alfa, false);
    REXCVAR_SET(nfsmw_dk_sombras_sesgo_pitch, 960);
    // Telemetry (automatic captures, frame dumps, per-draw traces of light frames, CPU stack sampling) only with
    // telemetria.flag next to the NRO: it costs frames (the stack sampler stops every thread each millisecond).
    {
      std::error_code ec;
      // (A release build ignores it: CMakeLists.txt, DS2_DEPURACION.)
      telemetry_ = DS2_DEPURACION && std::filesystem::exists(rex::filesystem::GetExecutableFolder() / "telemetria.flag", ec);
      const auto folder = rex::filesystem::GetExecutableFolder() / "logs" / "rex";
      const auto stack_flag = folder / "perfil_pilas.flag";
      if (telemetry_ && REXCVAR_GET(ds2_perfil_pilas)) {
        std::filesystem::create_directories(folder, ec);
        if (FILE* flag = std::fopen(stack_flag.string().c_str(), "a")) std::fclose(flag);
      } else {
        std::filesystem::remove(stack_flag, ec);  // left by an earlier telemetry run
      }
      REXLOG_INFO("[gpu] telemetry {}", telemetry_ ? "on (telemetria.flag)" : "off");
      // The renderer's GPU profile of a frame every 20 s (a barrier and two timestamps around each of ~1300 draws, the
      // first draw of each pass drawn twice) and the frame structure traces (hundreds of log lines to the SD card once
      // a minute) are telemetry too: each was a visible hitch.
      if (!telemetry_) {
        REXCVAR_SET(nfsmw_dk_perfil_gpu_s, 0);
      }
      hide_glow_ = DS2_DEPURACION && std::filesystem::exists(rex::filesystem::GetExecutableFolder() / "sin_resplandor.flag", ec);
      if (hide_glow_) REXLOG_INFO("[gpu] sin_resplandor.flag: lamp glows and coronas are not drawn");
    }
    // Before the game creates its shaders (the create hooks look the containers up in it).
    ds2::shaders::Global().Cargar(rex::filesystem::GetExecutableFolder() / "ds2_shaders.dslib");
#endif
    last_report_ = Clock::now();
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU vblank");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring");
    ring_thread_->Create();
    REXLOG_INFO("[gpu] ring reader started");
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[gpu] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    // As in CommandProcessor::InitializeRingBuffer: (1 << (size_log2 + 3)) bytes.
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[gpu] ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_pointer_writeback_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    if (!active_.exchange(false)) {
      DestroyPresentation();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(ring_mutex_);
    }
    ring_cv_.notify_all();
    if (ring_thread_) {
      ring_thread_->Wait(0, 0, 0, nullptr);
      ring_thread_.reset();
    }
    if (vblank_thread_) {
      vblank_thread_->Wait(0, 0, 0, nullptr);
      vblank_thread_.reset();
    }
    DestroyPresentation();
  }

  // After the threads: the presenter goes before its provider, on the UI thread.
  void DestroyPresentation() {
    destinos_.reset();  // frees its deko3d memory before the device goes
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- MMIO registers ---------------------------------------------------------

  static uint32_t ReadMmio(void* ppc_context, void* context, uint32_t address) {
    (void)ppc_context;
    return static_cast<RingGraphicsSystem*>(context)->ReadMmioRegister(address);
  }

  static void WriteMmio(void* ppc_context, void* context, uint32_t address, uint32_t value) {
    (void)ppc_context;
    static_cast<RingGraphicsSystem*>(context)->WriteMmioRegister(address, value);
  }

  // The same fixed values as the SDK's emulation (graphics/graphics_system.cpp).
  uint32_t ReadMmioRegister(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;  // in vblank
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        const uint32_t width = std::min(uint32_t(mode.display_width), uint32_t(0x0FFF));
        const uint32_t height = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
        return (width << 16) | height;
      }
      default:
        return Register(r);
    }
  }

  void WriteMmioRegister(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r < kRegisterCount) {
      registers_[r] = value;  // no side effects through MMIO (graphics_system.cpp)
    }
    if (r == kRegCpRbWptr) {
      write_pointer_.store(value, std::memory_order_release);
      // Taking the lock for an instant avoids losing the wakeup if the ring thread is between checking
      // and going to sleep.
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_one();
    }
  }

  uint32_t Register(uint32_t index) const { return index < kRegisterCount ? registers_[index] : 0; }

  // Register writes from the ring, with the side effects the game relies on.
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) {
      return;
    }
    if (index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
      value |= UINT32_C(0x80000000);
    }
#if DS2_DEKO3D
    // What the renderer caches per generation (PeticionDibujo): bumped only when a value really changes.
    if (registers_[index] != value) {
      if (index >= 0x4000 && index < 0x4800) {
        ++(index < 0x4400 ? generacion_constantes_vs_ : generacion_constantes_ps_);
      } else if (index >= 0x4800 && index <= 0x48BF) {
        ++generacion_fetch_;
      } else if (EsRegistroDeEncuadre(index)) {
        ++generacion_encuadre_;
      }
    }
#endif
    registers_[index] = value;
    // Scratch registers are mirrored to memory (SCRATCH_ADDR) for the ones SCRATCH_UMSK selects: the game
    // waits on those values to know where the GPU is.
    if (index >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 && index <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = index - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & registers_[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
        const uint32_t address = registers_[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address), value);
        GuestSeesGpuProgress();
      }
    }
  }

  // The guest can see the GPU move on (a fence value written to memory, a scratch register mirrored, an interrupt):
  // from there it may rewrite vertex and index memory the GPU already drew from. Dead Space 2 refills one dynamic
  // vertex buffer for its translucent draws this way, several times a frame at the same address. The renderer keeps
  // copies of ranges it already uploaded (in the work slot, and per epoch in the geometry store) and forgets them
  // when this counter moves; without it every translucent batch after the first was drawn with the first one's
  // vertices: the huge triangles across the screen.
  static void GuestSeesGpuProgress() {
#if DS2_DEKO3D
    rex::x360::g_sincronizaciones_anillo.fetch_add(1, std::memory_order_relaxed);
#endif
  }

  // --- GPU memory (the two low bits of the address are the byte order) --------

  uint32_t ReadMemory(uint32_t address) const {
    uint32_t value;
    std::memcpy(&value, memory_->TranslatePhysical(address & ~uint32_t(0x3)), sizeof(value));
    return xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    GuestSeesGpuProgress();
    const uint32_t swapped = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~uint32_t(0x3)), &swapped, sizeof(swapped));
  }

  // --- Threads ----------------------------------------------------------------

  // Creates the deko renderer backend on first use (on the ring thread, as the device allows). The game plugs in no
  // effects, so it uses the default hooks.
  bool EnsureRenderer() {
    if (destinos_) {
      return true;
    }
    if (!provider_ || !presenter_) {
      return false;
    }
    destinos_ = rex::x360::CrearDestinosDeko3d(
        static_cast<rex::deko3d::ProveedorDeko3d*>(provider_.get()), memory_);
    if (destinos_) {
      REXLOG_INFO("[gpu] deko3d renderer backend up (minimal renderer: draws, resolves and the Swap's frontbuffer)");
    }
    return destinos_ != nullptr;
  }

  // Runs the game's interrupt callback on the calling thread (one of ours, an XHostThread).
  void Interrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) {
      return;
    }
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) {
      return;
    }
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (active_.load(std::memory_order_acquire)) {
      const auto now = Clock::now();
      // After a long pause (loading, a debugger) the missed vblanks are not replayed.
      if (now - next > std::chrono::milliseconds(250)) {
        next = now;
      }
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  int RingLoop() {
    uint32_t read = 0;
    uint32_t generation = 0;
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(4), [&] {
          return !active_.load(std::memory_order_acquire) ||
                 write_pointer_.load(std::memory_order_acquire) != read ||
                 ring_generation_.load(std::memory_order_acquire) != generation;
        });
      }
      if (!active_.load(std::memory_order_acquire)) {
        break;
      }
      Report();
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;  // InitializeRingBuffer leaves the read pointer at zero
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) {
        continue;
      }
      const uint32_t written = write_pointer_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) {
        continue;
      }
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
      while (reader.Pending()) {
        if (!Packet(reader, 0)) {
          break;  // partial packet: the rest comes with the next CP_RB_WPTR
        }
      }
      read = reader.pos;
#if DS2_DEKO3D
      if (destinos_) {
        destinos_->EsperarSubidas();
      }
#endif
      const uint32_t writeback = read_pointer_writeback_.load(std::memory_order_acquire);
      if (writeback) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read);
        // Not a GuestSeesGpuProgress: it is written after every batch of the ring, and each one made the renderer's
        // geometry store check every range again (an epoch). Ring memory comes back only after a wrap, frames later.
      }
    }
    return 0;
  }

  // One line every kReportEvery, on the ring thread, so the log shows whether the game is advancing.
  void Report() {
    const auto now = Clock::now();
    if (now - last_report_ < kReportEvery) {
      return;
    }
    const double seconds = std::chrono::duration<double>(now - last_report_).count();
    last_report_ = now;
    REXLOG_INFO("[gpu] {:.1f} s: {} swaps ({:.1f}/s), {} draws, {} packets, {} vblanks, {} waits cut short",
                seconds, swaps_ - reported_swaps_, double(swaps_ - reported_swaps_) / seconds,
                draws_ - reported_draws_, packets_ - reported_packets_,
                vblanks_.load(std::memory_order_relaxed), waits_cut_short_);
    reported_swaps_ = swaps_;
    reported_draws_ = draws_;
    reported_packets_ = packets_;
    REXLOG_INFO("[gpu] ring waits: {} WAIT_REG_MEM waited {:.1f} ms in total", reg_mem_waits_,
                double(reg_mem_wait_ns_) / 1e6);
    reg_mem_waits_ = 0;
    reg_mem_wait_ns_ = 0;
#if DS2_DEKO3D
    const ds2::shaders::Estadisticas s = ds2::shaders::Global().estadisticas();
    REXLOG_INFO("[gpu] shaders: {} loads (address {}, address refused {}, learned {}, microcode {}, similar {}, "
                "unknown {}); objects {} ({} not in the library, {} without a microcode address)",
                s.cargas, s.por_direccion, s.direccion_rechazada, s.por_aprendizaje, s.por_microcodigo,
                s.por_parecido, s.desconocidas,
                s.objetos, s.objetos_sin_entrada, s.objetos_sin_direccion);
    // What the draws sent to the renderer target and draw (ColorRenderTargetFormat of each written color
    // target, DepthRenderTargetFormat when depth is on, primitive type): the formats it does not handle yet show
    // up here with how many draws they cost.
    std::string formats;
    for (uint32_t f = 0; f < 16; ++f) {
      if (color_formats_[f]) formats += fmt::format(" c{}:{}", f, color_formats_[f]);
    }
    for (uint32_t f = 0; f < 2; ++f) {
      if (depth_formats_[f]) formats += fmt::format(" z{}:{}", f, depth_formats_[f]);
    }
    std::string primitives;
    for (uint32_t t = 0; t < 32; ++t) {
      if (primitive_types_[t]) primitives += fmt::format(" {}:{}", t, primitive_types_[t]);
    }
    std::string orders;
    static constexpr const char* kOrder[4] = {"none", "8in16", "8in32", "16in32"};
    for (uint32_t k = 0; k < 8; ++k) {
      if (index_orders_[k]) orders += fmt::format(" {}-bit {}:{}", k < 4 ? 16 : 32, kOrder[k & 3], index_orders_[k]);
    }
    index_orders_ = {};
#if DS2_DEKO3D
    const uint32_t syncs = rex::x360::g_sincronizaciones_anillo.load(std::memory_order_relaxed);
    const uint32_t syncs_interval = syncs - syncs_reported_;
    syncs_reported_ = syncs;
#else
    const uint32_t syncs_interval = 0;
#endif
    REXLOG_INFO("[gpu] draw targets:{}; primitive types:{}; indices:{}; {} emulated clears, {} multisampled draws drawn "
                "at 1x; {} guest-visible GPU progress points (renderer copies forgotten)", formats, primitives, orders,
                clears_, msaa_draws_, syncs_interval);
    color_formats_ = {};
    depth_formats_ = {};
    primitive_types_ = {};
    uint64_t dibujados = 0, rechazados = 0;
    if (destinos_) {
      const rex::x360::EstadisticasDibujos e = destinos_->EstadisticasDeDibujos();
      dibujados = e.dibujados;
      rechazados = e.rechazados;
    }
    REXLOG_INFO("[gpu] draws: {} geometry, {} sent to the renderer: {} drawn, {} rejected by it; skipped here: "
                "{} without VS, {} without PS (the renderer logs each rejection cause once)",
                geometria_, enviados_, dibujados, rechazados, sin_vs_, sin_ps_);
#endif
  }

  // --- PM4 packets ------------------------------------------------------------

  // Consumes a whole packet, or leaves the position untouched if it is not complete yet.
  bool Packet(Reader& reader, int depth) {
    const uint32_t packet = reader.Peek();
    uint32_t count = 0;
    switch (packet >> 30) {
      case 0: count = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0; break;
      case 1: count = 2; break;
      case 2: count = 0; break;
      default: count = ((packet >> 16) & 0x3FFF) + 1; break;
    }
    if (reader.Pending() < count + 1) {
      return false;
    }
    reader.Skip(1);
    ++packets_;
    Reader data = reader;
    reader.Skip(count);
    if (!packet) {
      return true;
    }
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool one_register = (packet >> 15) & 0x1;
        for (uint32_t i = 0; i < count; ++i) {
          WriteRegister(one_register ? index : index + i, data.Read());
        }
        break;
      }
      case 1: {
        const uint32_t value1 = data.Read();
        const uint32_t value2 = data.Read();
        WriteRegister(packet & 0x7FF, value1);
        WriteRegister((packet >> 11) & 0x7FF, value2);
        break;
      }
      case 2:
        break;
      default:
        PacketType3(data, packet, count, depth);
        break;
    }
    return true;
  }

  void PacketType3(Reader& data, uint32_t packet, uint32_t count, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
#if DS2_DEKO3D
      ++frame_.predicated_skipped;
#endif
      return;  // predicate fails; predicated Swaps are always dropped
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (count < 1) break;
        const uint32_t cpus = data.Read();
        GuestSeesGpuProgress();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) {
            Interrupt(1, cpu);
          }
        }
        break;
      }
      case xenos::PM4_XE_SWAP:
#if DS2_DEKO3D
        FrameTraceSwap();
#endif
        // VdSwap writes "SWAP", the frontbuffer's physical address and its size. The renderer presents whatever is
        // resolved (clears and, once the ring decoder feeds draws, the frame). Handheld 1280x720.
        ++swaps_;
        counter_.fetch_add(1, std::memory_order_relaxed);
        if (EnsureRenderer()) {
          // VdSwap describes the frontbuffer in fetch constant 0; an empty TexturaSwap finds nothing (black).
          {
            std::lock_guard<std::mutex> cerrojo(g_rampa_cerrojo);
            if (g_rampa_nueva) {
              g_rampa_nueva = false;
              destinos_->RampaGamma(g_rampa);
              REXLOG_INFO("[gpu] gamma ramp from the game: 0 -> {:03X}, 128 -> {:03X}, 255 -> {:03X} (of 3FF)",
                          g_rampa[0][0], g_rampa[128][0], g_rampa[255][0]);
            }
          }
          destinos_->Presentar(presenter_.get(), rex::x360::LeerTexturaSwap(registers_.data()), 1280, 720);
          if (REXCVAR_GET(switch_omitir_pedido)) {
            REXCVAR_SET(switch_omitir_pedido, false);
            skip_mode_ = ((skip_mode_ ? skip_mode_ : skip_resume_) + 1) % kSkipModes;
            skip_resume_ = 0;
            SetNfsShortcuts(skip_mode_ != 1);
            REXLOG_INFO("[gpu] skip mode {}: {} (the next capture shows it)", skip_mode_, kSkipModeNames[skip_mode_]);
            capture_after_skip_ = 3;  // a few frames later, when the change is on screen
          }
          if (capture_after_skip_ && --capture_after_skip_ == 0) {
            SaveCapture();
            REXLOG_INFO("[gpu] capture above: skip mode {} ({})", skip_mode_, kSkipModeNames[skip_mode_]);
            // The skipping modes hide part of the game (the ammo hologram is additive): back to normal rendering once
            // their capture is taken; the next press goes on to the next mode. Safe mode stays until the next press.
            if (skip_mode_ > 1) {
              skip_resume_ = skip_mode_;
              skip_mode_ = 0;
              REXLOG_INFO("[gpu] skip mode {} done, normal rendering again", skip_resume_);
            }
          }
          if (REXCVAR_GET(switch_volcado_pedido)) {
            REXCVAR_SET(switch_volcado_pedido, false);
            destinos_->PedirVolcado();
            // And every draw of that frame in the log, whatever its size (FrameTraceSwap ends it at the next Swap).
            draw_tracing_ = true;
            draw_trace_n_ = 0;
            draw_trace_cap_ = 3000;
            REXLOG_INFO("[draws] full trace of the next frame (ZL + ZR + Left)");
          }
          AutomaticCollection();
          if (REXCVAR_GET(switch_captura_pedida)) {
            REXCVAR_SET(switch_captura_pedida, false);
            SaveCapture();
          }
        }
        break;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (count < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxIndirectDepth) {
          Reader indirect;
          indirect.base = memory_->TranslatePhysical(address);
          indirect.end = length;
          while (indirect.Pending() && Packet(indirect, depth + 1)) {
          }
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (count < 5) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const auto start = Clock::now();
        const auto limit = start + kWaitRegMemMax;
        const auto yield_until = start + std::chrono::milliseconds(1);
        bool waited = false;
        for (;;) {
          if (!(info & 0x10) && poll == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST &&
              (Register(poll) & UINT32_C(0x80000000))) {
            registers_[poll] = 0;  // MakeCoherent: there is no GPU cache to flush
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, value & mask, reference)) {
            break;
          }
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (waits_cut_short_++ == 0) {
              REXLOG_WARN("[gpu] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X}), going on",
                          (info & 0x10) ? "memory" : "register", poll, reference, mask);
            }
            break;
          }
          waited = true;
          // The first millisecond yielding instead of sleeping: a sleep on Horizon often lasts far longer than asked,
          // and the game's main thread waits on the GPU fences behind this wait (D3D BlockOnFence in its profile).
          if (Clock::now() < yield_until) {
            rex::thread::MaybeYield();
          } else {
            rex::thread::Sleep(std::chrono::microseconds(100));
          }
        }
        if (waited) {
          ++reg_mem_waits_;
          reg_mem_wait_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (count < 3) break;
        const uint32_t info = data.Read();
        const uint32_t and_value = data.Read();
        const uint32_t or_value = data.Read();
        uint32_t value = Register(info & 0x1FFF);
        value &= ((info >> 31) & 0x1) ? Register(and_value & 0x1FFF) : and_value;
        value |= ((info >> 30) & 0x1) ? Register(or_value & 0x1FFF) : or_value;
        WriteRegister(info & 0x1FFF, value);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (count < 2) break;
        const uint32_t reg = data.Read();
        const uint32_t address = data.Read();
        WriteMemory(address, Register(reg));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (count < 1) break;
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < count; ++i) {
          WriteMemory(address, data.Read());
          address += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (count < 6) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const uint32_t target = data.Read();
        const uint32_t value_to_write = data.Read();
        const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
        if (Compare(info, value & mask, reference)) {
          if (info & 0x100) {
            WriteMemory(target, value_to_write);
          } else {
            WriteRegister(target, value_to_write);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (count < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (count < 3) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        const uint32_t value = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1) ? counter_.load(std::memory_order_relaxed) : value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (count < 2) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // Screen extent of the previous draw: the maximum, as the SDK does.
        const uint16_t extent[] = {0, kMaxExtent, 0, kMaxExtent, 0, 1};
        uint8_t* target = memory_->TranslatePhysical(address & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extent); ++i) {
          rex::memory::store_and_swap<uint16_t>(target + i * 2, extent[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (count < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        OcclusionQuery(Register(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR));
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (count < 1) break;
        const uint32_t type_index = data.Read();
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 1; i < count; ++i) {
            WriteRegister(index + i - 1, data.Read());
          }
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        // First word: the register index; the rest is written consecutively.
        if (count < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        for (uint32_t i = 1; i < count; ++i) {
          WriteRegister(index + i - 1, data.Read());
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        // Registers loaded from guest memory instead of from the ring.
        if (count < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t n = data.Read() & 0xFFF;
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          const uint8_t* source = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 0; i < n; ++i) {
            WriteRegister(index + i, rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4));
          }
        }
        break;
      }
      case xenos::PM4_VIZ_QUERY: {
        // When a query ends it is reported as visible, in case the game reads the result (as the SDK does).
        if (count < 1) break;
        const uint32_t word = data.Read();
        const uint32_t id = word & 0x3F;
        if (!(word & 0x100)) {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
        } else {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
          const uint32_t reg = id < 32 ? uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0)
                                       : uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);
          registers_[reg] |= UINT32_C(1) << (id < 32 ? id : id - 32);
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (count < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (count < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (count < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (count < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        if (count < 2) break;
        const uint64_t high = data.Read();
        bin_mask_ = (high << 32) | data.Read();
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        if (count < 2) break;
        const uint64_t high = data.Read();
        bin_select_ = (high << 32) | data.Read();
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        ++draws_;
#if DS2_DEKO3D
        // DRAW_INDX starts with the visibility query word. Then the initiator and, for indices in memory (DMA),
        // their base and size, kept in their registers as the SDK's command processor does: the renderer reads
        // everything from the register mirror.
        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (count < skip + 1) break;
        if (skip) data.Read();
        const uint32_t initiator = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)) {
          if (count < skip + 3) break;
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_BASE, data.Read());
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE, data.Read());
        }
        if (!EnsureRenderer()) break;
        // A copy-mode draw is a resolve and/or a clear.
        if (rex::x360::EsCopia(registers_.data())) {
          ResolveCatalog();
          FrameTraceCopy();
          if (draw_tracing_ && draw_trace_n_ < draw_trace_cap_) {
            namespace g = rex::graphics;
            const uint32_t control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
            REXLOG_INFO("[draws]   copy: source {} (c0 {:03X} z {:03X}) pitch {} clear color {} depth {} command {} -> "
                        "{:08X} pitch {} height {} format {}",
                        control & 0x7, Register(g::XE_GPU_REG_RB_COLOR_INFO) & 0xFFF,
                        Register(g::XE_GPU_REG_RB_DEPTH_INFO) & 0xFFF,
                        Register(g::XE_GPU_REG_RB_SURFACE_INFO) & 0x3FFF, (control >> 8) & 1, (control >> 9) & 1,
                        (control >> 20) & 3, Register(g::XE_GPU_REG_RB_COPY_DEST_BASE),
                        Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH) & 0x3FFF,
                        (Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH) >> 16) & 0x3FFF,
                        (Register(g::XE_GPU_REG_RB_COPY_DEST_INFO) >> 7) & 0x3F);
          }
          destinos_->Copiar(rex::x360::LeerRegistrosCopia(registers_.data()));
          break;
        }
        Draw();
#else
        (void)data;
#endif
        break;
      }
#if DS2_DEKO3D
      case xenos::PM4_IM_LOAD: {
        // Microcode in GPU memory; the type (0 VS, 1 PS) in the two low bits of the address.
        if (count < 2) break;
        const uint32_t address_type = data.Read();
        const uint32_t words = data.Read() & 0xFFFF;
        const uint32_t type = address_type & 0x3;
        if (type > 1 || !words) break;
        const uint8_t* source = memory_->TranslatePhysical(address_type & ~uint32_t(0x3));
        microcode_.resize(words);
        for (uint32_t i = 0; i < words; ++i) {
          microcode_[i] = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
        }
        ShaderLoaded(type, address_type & ~uint32_t(0x3));
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        // Microcode inside the packet (D3D's copy path, patched there).
        if (count < 2) break;
        const uint32_t type = data.Read() & 0x3;
        const uint32_t words = data.Read() & 0xFFFF;
        if (type > 1 || !words || words > count - 2) break;
        microcode_.resize(words);
        for (uint32_t i = 0; i < words; ++i) {
          microcode_[i] = data.Read();
        }
        ShaderLoaded(type, 0);
        break;
      }
#endif
      default:
        // IM_LOAD, IM_LOAD_IMMEDIATE, INVALIDATE_STATE, NOP...: nothing the game waits for.
        break;
    }
  }

#if DS2_DEKO3D
  // PA_SC_WINDOW_OFFSET..SCISSOR_BR, PA_CL_VPORT_*, PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL and
  // PA_SU_VTX_CNTL: the viewport/scissor state the renderer caches (as the SDK's command processor).
  static bool EsRegistroDeEncuadre(uint32_t index) {
    if (index - 0x2080 > 0x2302 - 0x2080) return false;
    return index <= 0x2082 || (index >= 0x210F && index <= 0x2114) || (index >= 0x2204 && index <= 0x2206) ||
           index == 0x2302;
  }

  // --- Draw-skipping modes (ZL + ZR + Right), to find which kind of draw makes an artifact: each press goes to the next
  // mode, logs it and takes a capture a few frames later. Classified by the blend of target 0 (RB_BLENDCONTROL0) and the
  // primitive, as the frame traces show Dead Space 2's draws.
  static constexpr uint32_t kSkipModes = 8;
  static constexpr const char* kSkipModeNames[kSkipModes] = {
      "none (normal rendering)",
      "SAFE MODE: the renderer's NFS shortcuts off, nothing skipped",
      "deferred lights (blend ONE+ONE, 01010101) and their stencil passes",
      "additive effects (SRC_ALPHA+ONE, 00010106)",
      "alpha-blended (SRC_ALPHA+INV_SRC_ALPHA: 00010706, 07060706)",
      "quad lists (prim 13)",
      "everything blended (blend other than 00010001)",
      "alpha-tested (RB_COLORCONTROL alpha test on)",
  };
  // The renderer's NFS shortcuts (see the declarations at the top): on as the SDK defaults, off in safe mode.
  static void SetNfsShortcuts(bool on) {
    REXCVAR_SET(nfsmw_nativo_saltar_invisibles, on);
    REXCVAR_SET(nfsmw_nativo_sin_ps_sin_color, on);
    REXCVAR_SET(nfsmw_nativo_sin_desenfoque, on);
    REXCVAR_SET(nfsmw_nativo_pcf_barato, on);
    REXCVAR_SET(nfsmw_nativo_sombra_minimo, on);
    REXCVAR_SET(nfsmw_nativo_z_temprana, on);
    REXCVAR_SET(nfsmw_nativo_cielo_aplazado, on);
    REXCVAR_SET(nfsmw_dk_constantes_por_diferencias, on);
    REXCVAR_SET(nfsmw_nativo_saltar_borrados_repetidos, on);
    REXCVAR_SET(nfsmw_nativo_resolver_sin_copia, on);
    REXCVAR_SET(nfsmw_nativo_intercambiar_sin_borrado, on);
    REXCVAR_SET(nfsmw_nativo_frontal_perezoso, on);
    REXCVAR_SET(nfsmw_dk_borrar_area_util, on);
    REXCVAR_SET(nfsmw_dk_borrado_color_aplazado, on);
    REXCVAR_SET(nfsmw_dk_borrado_z_aplazado, on);
  }

  bool SkippedByMode() const {
    namespace g = rex::graphics;
    const uint32_t blend = Register(g::XE_GPU_REG_RB_BLENDCONTROL0);
    const uint32_t mask = Register(g::XE_GPU_REG_RB_COLOR_MASK);
    const uint32_t prim = Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR) & 0x3F;
    const uint32_t colorctl = Register(g::XE_GPU_REG_RB_COLORCONTROL);
    // The debug modes (ZL + ZR + Right) or, without one, the options menu's SKIP row (ds2_omitir_efectos).
    const uint32_t modo = skip_mode_ > 1 ? skip_mode_ : uint32_t(REXCVAR_GET(ds2_omitir_efectos)) + 1;
    switch (modo - 1) {  // mode 1 is the safe mode, which skips nothing
      case 1:
        // The light volumes add ONE+ONE; their stencil-marking passes write no color (mask 0) in depth-only mode.
        return blend == 0x01010101 || (!(mask & 0xF) && (Register(g::XE_GPU_REG_RB_DEPTHCONTROL) & 0x1));
      case 2: return (mask & 0xF) && blend == 0x00010106;
      case 3: return (mask & 0xF) && (blend == 0x00010706 || blend == 0x07060706);
      case 4: return prim == 13;
      case 5: return (mask & 0xF) && blend != 0x00010001;
      case 6: return ((colorctl >> 3) & 0x1) && (colorctl & 0x7) != 7;
      default: return false;
    }
  }

  // The lamp glow (gVFGlowInfo) and corona (modulate_by_corona_info_vs) vertex shaders: both read the lamp's
  // visibility from a texture in the vertex shader (tfetch2DVertice) and build their quads from it. Suspects for the
  // streaks next to flickering lamps; sin_resplandor.flag next to the NRO leaves them out to tell.
  static bool IsGlowShader(const rex::x360::EntradaShader* entry) {
    return entry && (entry->huella_contenedor == UINT64_C(0x36929751F8A50138) ||
                     entry->huella_contenedor == UINT64_C(0xC94E83E0DB28EA74));
  }

  // Once per patched variant: the vertex fetches D3D wrote for the declaration (format, stride, offset), which the
  // container does not have.
  void LogGlowFetches(const rex::x360::EntradaShader* entry) {
    const uint64_t key = XXH3_64bits(microcode_.data(), microcode_.size() * sizeof(uint32_t));
    if (glow_logged_.size() >= 16 || !glow_logged_.insert(key).second) return;
    std::string fetches;
    for (size_t i = 0; i + 2 < microcode_.size(); i += 3) {
      const uint32_t w0 = microcode_[i], w1 = microcode_[i + 1], w2 = microcode_[i + 2];
      if ((w0 & 0x1F) != 0 || !((w0 >> 19) & 1)) continue;  // vertex fetch: opcode 0, must-be-one bit
      fetches += fmt::format(" [slot {} r{} const {}.{} format {} signed {} int {} stride {} offset {} swz {:03X}]",
                             i / 3, (w0 >> 12) & 0x3F, (w0 >> 20) & 0x1F, (w0 >> 25) & 3, (w1 >> 16) & 0x3F,
                             (w1 >> 14) & 1, (w1 >> 13) & 1, w2 & 0xFF, (w2 >> 8) & 0x7FFFFF, w1 & 0xFFF);
    }
    REXLOG_INFO("[gpu] glow VS {:016X} (n{}): {} words, fetches:{}", entry->huella_contenedor, entry->numero,
                microcode_.size(), fetches);
  }

  // An IM_LOAD / IM_LOAD_IMMEDIATE of type 0 (VS) or 1 (PS), its microcode in microcode_ (host order).
  void ShaderLoaded(uint32_t type, uint32_t physical) {
    const rex::x360::EntradaShader* entry = ds2::shaders::Global().Identificar(type == 0, physical, microcode_);
    if (type == 1) {
      ps_ = entry;
      return;
    }
    // The same vertex shader loaded again from the same address (D3D reloads it for every draw that sets it): its
    // patched microcode and generation are the ones already here. Checked by length and three words, as the library
    // does, instead of copying and hashing it again.
    const size_t n = microcode_.size();
    if (physical && entry && entry == vs_ && physical == vs_physical_ && n && n == vs_microcode_.size() &&
        microcode_[0] == vs_microcode_[0] && microcode_[n / 2] == vs_microcode_[n / 2] &&
        microcode_[n - 1] == vs_microcode_[n - 1]) {
      return;
    }
    vs_physical_ = physical;
    vs_ = entry;
    vs_microcode_ = microcode_;  // patched by D3D: the vertex input is read from here
    if (IsGlowShader(entry)) LogGlowFetches(entry);
    // The same microcode keeps its generation, so the renderer's per-generation caches stay valid.
    const uint64_t key = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t)) ^
                         vs_microcode_.size();
    auto [it, inserted] = vs_generations_.try_emplace(key, vs_generations_.size() + 1);
    generacion_vs_ = it->second;
  }

  void Draw() {
    ++geometria_;
    FrameTraceDraw();
    namespace g = rex::graphics;
    // Depth only (RB_MODECONTROL 5): the GPU runs no pixel shader, so the VS is enough.
    const bool depth_only = (Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5;
    const uint32_t msaa = (Register(g::XE_GPU_REG_RB_SURFACE_INFO) >> 16) & 0x3;
    // Which way the scene's depth test goes, for the depth value of the emulated clears.
    if (!msaa && (Register(g::XE_GPU_REG_RB_DEPTHCONTROL) & 0x2)) {
      const uint32_t zfunc = (Register(g::XE_GPU_REG_RB_DEPTHCONTROL) >> 4) & 0x7;
      if (zfunc == 4 || zfunc == 6) ++depth_greater_; else if (zfunc == 1 || zfunc == 3) ++depth_less_;
    }
    // Direct3D clears a target by drawing over it with its own internal shaders (not created by the game, so not in
    // the library), through the same EDRAM seen as a multisampled surface of half the pitch. Done as the renderer's
    // clears instead.
    // Only full-surface ones (window scissor to 8192): Dead Space 2's light stencil masks are multisampled draws too,
    // and one whose VS was not identified, taken for a clear, wiped the stencil and let a light flood the screen.
    // A single-sampled full-surface draw without an identified VS is usually a real draw (the lamps' 64x8 visibility
    // pass is one, and clearing for it wiped the scene's depth in the middle of the frame). Single-sampled, only
    // Direct3D's own clear shape is taken: one rectangle (rect list, 3 vertices) with all four targets' mask. The
    // decompression scene clears its quarter-size particle buffer that way every frame; left as it was, the smoke and
    // debris of every frame piled up into grey smears. And only onto an 8-bit color target with a color in 0..1 in the
    // clear shader's c0: the same shape also fills the 80-wide float exposure target with game data (c0 like 0.027 0
    // -18.4 1), which is not a clear.
    const uint32_t initiator = Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    bool rect_clear = !msaa && (initiator & 0x3F) == 8 && (initiator >> 16) == 3 &&
                      Register(g::XE_GPU_REG_RB_COLOR_MASK) == 0xFFFF &&
                      ((Register(g::XE_GPU_REG_RB_COLOR_INFO) >> 16) & 0xF) <= 1;
    for (uint32_t j = 0; j < 4 && rect_clear; ++j) {
      float v;
      std::memcpy(&v, &registers_[0x4400 + j], 4);
      rect_clear = v >= 0.0f && v <= 1.0f;
    }
    if (!vs_ && ((msaa && (Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR) & 0x3FFF) >= 0x2000) || rect_clear)) {
      if (draw_tracing_) DrawTraceLine(true, "Direct3D clear, emulated");
      EmulateClear(msaa);
      return;
    }
    if (!vs_) {
      ++sin_vs_;
      // A real draw whose VS was not identified (not a Direct3D clear): which microcode, once each.
      if (!msaa && unknown_vs_logged_.size() < 16) {
        const uint64_t key = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
        if (unknown_vs_logged_.insert(key).second) {
          REXLOG_WARN("[gpu] draw skipped, VS not identified: microcode {:016X} ({} words, first {:08X} {:08X} "
                      "{:08X}), target c0 {:08X} pitch {}, PS n{}",
                      key, vs_microcode_.size(), vs_microcode_.size() > 0 ? vs_microcode_[0] : 0,
                      vs_microcode_.size() > 1 ? vs_microcode_[1] : 0, vs_microcode_.size() > 2 ? vs_microcode_[2] : 0,
                      Register(g::XE_GPU_REG_RB_COLOR_INFO), Register(g::XE_GPU_REG_RB_SURFACE_INFO) & 0x3FFF,
                      ps_ ? int(ps_->numero) : -1);
        }
      }
      if (draw_tracing_) DrawTraceLine(false, "no VS");
      return;
    }
    if (!ps_ && !depth_only) {
      ++sin_ps_;
      return;
    }
    if ((skip_mode_ > 1 || REXCVAR_GET(ds2_omitir_efectos) > 0) && SkippedByMode()) {
      ++skipped_by_mode_;
      return;
    }
    if (IsGlowShader(vs_)) {
      if (glow_draws_logged_ < 12) {
        ++glow_draws_logged_;
        // Fetch constant 17 (vertex texture register 1): the visibility texture the shader reads.
        const uint32_t f = 0x4800 + 17 * 6;
        // Vertex fetch constants 31.0-31.2 (where the containers point their fetches) share fetch slot 31's words.
        const uint32_t v = 0x4800 + 31 * 6;
        REXLOG_INFO("[gpu] glow draw (VS n{}, PS n{}): texture fetch 17 {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}; "
                    "vertex fetch 31.x {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                    vs_->numero, ps_ ? ps_->numero : 0, Register(f), Register(f + 1), Register(f + 2),
                    Register(f + 3), Register(f + 4), Register(f + 5), Register(v), Register(v + 1), Register(v + 2),
                    Register(v + 3), Register(v + 4), Register(v + 5));
      }
      if (hide_glow_) return;
    }
    // Diagnostics (Report): formats and primitive types of what is sent.
    const uint32_t mask = Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK);
    static constexpr uint32_t kColorInfo[4] = {
        rex::graphics::XE_GPU_REG_RB_COLOR_INFO, rex::graphics::XE_GPU_REG_RB_COLOR1_INFO,
        rex::graphics::XE_GPU_REG_RB_COLOR2_INFO, rex::graphics::XE_GPU_REG_RB_COLOR3_INFO};
    for (uint32_t i = 0; i < 4 && ps_ && !depth_only; ++i) {
      if (((mask >> (i * 4)) & 0xF) && ((ps_->salidas >> i) & 0x1)) {
        ++color_formats_[(Register(kColorInfo[i]) >> 16) & 0xF];
      }
    }
    if (Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL) & 0x6) {  // Z test or Z write
      ++depth_formats_[(Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO) >> 16) & 0x1];
    }
    ++primitive_types_[Register(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR) & 0x1F];
    {
      // Indexed draws by index size and byte order (VGT_DMA_SIZE bits 30-31): 16-bit indices in k8in32 or k16in32
      // were read unswapped by the SDK.
      const uint32_t init = Register(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR);
      if (((init >> 6) & 0x3) == 0) {  // source select: DMA (an index buffer)
        ++index_orders_[((init >> 11) & 0x1) * 4 + (Register(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE) >> 30)];
      }
    }

    rex::x360::PeticionDibujo request;
    request.registros = registers_.data();
    AliasScope alias(*this);
    if (msaa) {
      // Direct3D's trick for quarter-rate passes (Dead Space 2's light stencil masks): the 1280x720 depth buffer
      // drawn as a 640x360 4x-multisampled surface over the same EDRAM, each pixel's four samples landing on 2x2
      // pixels. The renderer has no multisampling and keys targets by pitch, so the draw goes to the 1x surface
      // with everything in pixels scaled up instead (same pixels covered).
      request.registros = alias.Apply(msaa);
      ++generacion_encuadre_;  // the scaled viewport and scissor are not the mirror's
    }
    request.vs = vs_;
    request.ps = ps_;
    request.vs_microcodigo = vs_microcode_;
    request.generacion_vs = generacion_vs_;
    request.generacion_constantes_vs = generacion_constantes_vs_;
    request.generacion_constantes_ps = generacion_constantes_ps_;
    request.generacion_fetch = generacion_fetch_;
    request.generacion_encuadre = generacion_encuadre_;
    ++enviados_;
    const bool drawn = destinos_->Dibujar(request);
    if (draw_tracing_) DrawTraceLine(drawn);
    if (msaa) {
      ++generacion_encuadre_;  // back to the mirror's for the next draw
      ++msaa_draws_;
    }
  }

  // The register mirror with a multisampled surface turned into the 1x surface it aliases: 4X doubles width and
  // height (pitch x2), 2X doubles the height (samples stacked vertically). Viewport and scissors scale the same way.
  // Done in place on the ten registers involved and undone when the scope ends (copying the whole 80 KB mirror for
  // each of the ~30 multisampled draws per frame cost ~2.4 MB of copies per frame on the ring thread).
  class AliasScope {
   public:
    explicit AliasScope(RingGraphicsSystem& system) : system_(system) {}
    ~AliasScope() {
      for (uint32_t i = 0; i < count_; ++i) system_.registers_[saved_[i].first] = saved_[i].second;
    }
    const uint32_t* Apply(uint32_t msaa) { return system_.AliasedRegisters(msaa, *this); }
    void Save(uint32_t index) { saved_[count_++] = {index, system_.registers_[index]}; }

   private:
    RingGraphicsSystem& system_;
    std::pair<uint32_t, uint32_t> saved_[12];
    uint32_t count_ = 0;
  };

  const uint32_t* AliasedRegisters(uint32_t msaa, AliasScope& scope) {
    namespace g = rex::graphics;
    for (uint32_t index : {uint32_t(g::XE_GPU_REG_RB_SURFACE_INFO), uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE),
                           uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 1), uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 2),
                           uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 3), uint32_t(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL),
                           uint32_t(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR), uint32_t(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL),
                           uint32_t(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR)}) {
      scope.Save(index);
    }
    uint32_t* r = registers_.data();
    const uint32_t sx = msaa == 2 ? 2 : 1, sy = 2;
    const uint32_t surface = r[g::XE_GPU_REG_RB_SURFACE_INFO];
    r[g::XE_GPU_REG_RB_SURFACE_INFO] = (surface & ~uint32_t(0x33FFF)) | std::min<uint32_t>((surface & 0x3FFF) * sx, 0x3FFF);
    auto scale_float = [&](uint32_t index, uint32_t factor) {
      float v;
      std::memcpy(&v, &r[index], 4);
      v *= float(factor);
      std::memcpy(&r[index], &v, 4);
    };
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE, sx);
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 1, sx);  // XOFFSET
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 2, sy);  // YSCALE
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 3, sy);  // YOFFSET
    auto scale_xy = [&](uint32_t index, uint32_t bits) {
      const uint32_t v = r[index];
      const uint32_t mask = (1u << bits) - 1;
      const uint32_t x = std::min((v & mask) * sx, mask), y = std::min(((v >> 16) & mask) * sy, mask);
      r[index] = (v & ~(mask | (mask << 16))) | x | (y << 16);
    };
    scale_xy(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, 14);
    scale_xy(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, 14);
    scale_xy(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL, 15);
    scale_xy(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR, 15);
    return r;
  }

  // A Direct3D clear draw, as the renderer's clears. Direct3D's fast clear binds c0 (the color to clear) and, for the
  // depth buffer, c1-c3 over the depth buffer's EDRAM written as color, with a placeholder depth target that may even
  // sit on c0's base (Dead Space 2's light buffer clear: c0 2D0, c1-c3 000, depth 2D0). So: c0 is cleared as color
  // (to 0); c1-c3 on another base than c0 clear the depth buffer at their base (far plane, stencil 0); the bound
  // depth target is cleared only when depth or stencil is written and it is not on c0's base; depth-only mode
  // clears no color. The first few are logged with what the game had set, to check.
  void EmulateClear(uint32_t msaa) {
    namespace g = rex::graphics;
    AliasScope alias(*this);
    const uint32_t* r = msaa ? alias.Apply(msaa) : registers_.data();
    rex::x360::RegistrosCopia base = rex::x360::LeerRegistrosCopia(r);
    // The whole surface: the renderer takes a copy's rectangle from three vertices in fetch constant 0 (where
    // Direct3D puts a resolve's), clipped by the window scissor. Ours, in a small guest buffer made once.
    if (!clear_rect_) {
      clear_rect_ = memory_->SystemHeapAlloc(32, 32, rex::memory::kSystemHeapPhysical);
    }
    if (!clear_rect_) return;
    const float w = float(r[g::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF), h = 8192.0f;
    const float rect[6] = {0.0f, 0.0f, w, 0.0f, 0.0f, h};  // host order, read with Endian::kNone
    std::memcpy(memory_->TranslateVirtual<uint8_t*>(clear_rect_), rect, sizeof(rect));
    const uint32_t physical = memory_->GetPhysicalAddress(clear_rect_);
    base.fetch_vertices[0] = (physical & ~uint32_t(0x3)) | uint32_t(xenos::FetchConstantType::kVertex);
    base.fetch_vertices[1] = (6u << 2) | uint32_t(xenos::Endian::kNone);
    base.pa_sc_window_offset = 0;
    base.pa_sc_window_scissor_tl = 0x80000000u;  // window_offset_disable, from 0,0
    base.pa_sc_window_scissor_br = 0x3FFF3FFFu;
    base.pa_su_sc_mode_cntl &= ~(1u << 16);       // no vtx_window_offset
    const uint32_t mask = r[g::XE_GPU_REG_RB_COLOR_MASK];
    const uint32_t depth_info = r[g::XE_GPU_REG_RB_DEPTH_INFO];
    const uint32_t depth_control = r[g::XE_GPU_REG_RB_DEPTHCONTROL];
    const bool depth_only = (r[g::XE_GPU_REG_RB_MODECONTROL] & 0x7) == 5;
    const bool reversed = depth_greater_ > depth_less_;
    // The stencil: Direct3D's clear quad writes the stencil reference with a REPLACE (depthcontrol 00008777: stencil on,
    // ALWAYS, z-pass REPLACE), not 0. Dead Space 2 clears it to its own value before the menu's Scaleform masks, whose
    // frames test LESS against it; written as 0, every frame and backing panel of the menus failed and only the text
    // (drawn without stencil) was left.
    uint32_t stencil = 0;
    if ((depth_control & 0x1) && ((depth_control >> 14) & 0x7) == 2) {
      const uint32_t ref_mask = r[g::XE_GPU_REG_RB_STENCILREFMASK];
      stencil = ref_mask & (ref_mask >> 16) & 0xFF;
    }
    const uint32_t depth_value = (reversed ? 0u : 0xFFFFFF00u) | stencil;
    constexpr uint32_t kNullCommand = 3u << 20;  // CopyCommand::kNull: clear only, copy nothing
    auto clear_depth_at = [&](uint32_t edram_base, std::string& what) {
      rex::x360::RegistrosCopia c = base;
      c.rb_depth_info = (depth_info & ~uint32_t(0xFFF)) | (edram_base & 0xFFF);
      c.rb_copy_control = 4u | (1u << 9) | kNullCommand;
      c.rb_depth_clear = depth_value;
      destinos_->Copiar(c);
      what += fmt::format(" z={:03X}->{:08X}", edram_base & 0xFFF, depth_value);
    };
    std::string what;
    const uint32_t c0_base = base.rb_color_info[0] & 0xFFF;
    const bool c0_written = !depth_only && (mask & 0xF);
    // The color: the multisampled ones were all seen clearing to 0. A single-sampled one takes what Direct3D's clear
    // shader writes, the pixel shader's c0 (RGBA8 for the renderer's clear).
    uint32_t color = 0;
    if (!msaa) {
      for (uint32_t j = 0; j < 4; ++j) {
        float v;
        std::memcpy(&v, &registers_[0x4400 + j], 4);
        color |= uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)) << (j * 8);
      }
    }
    if (c0_written) {
      rex::x360::RegistrosCopia c = base;
      c.rb_copy_control = 0u | (1u << 8) | kNullCommand;
      c.rb_color_clear = color;
      c.rb_color_clear_lo = 0;
      destinos_->Copiar(c);
      what += fmt::format(" c0={:03X}", c0_base);
      if (color) what += fmt::format("={:08X}", color);
    }
    uint32_t aliased_depth[3] = {};
    uint32_t n_aliased = 0;
    for (uint32_t i = 1; i < 4 && !depth_only; ++i) {
      const uint32_t b = base.rb_color_info[i] & 0xFFF;
      if (!((mask >> (i * 4)) & 0xF) || (c0_written && b == c0_base)) continue;
      // Only targets the clear's PS exports: the Xenos leaves the others alone (ds3-nx, run 16). Direct3D can leave
      // c1-c3 bound on a base with mask FFFF while its PS writes oC0 only; taken as depth clears, a color clear wiped
      // the scene's depth and stencil there. At 1280x720 those bases happened to miss the scene depth; at 960x540 the
      // game lays its EDRAM out differently and the scene came out see-through, textures over the wrong surfaces.
      if (REXCVAR_GET(ds2_borrado_solo_salidas) && ps_ && !((ps_->salidas >> i) & 0x1)) continue;
      if (std::find(aliased_depth, aliased_depth + n_aliased, b) != aliased_depth + n_aliased) continue;
      aliased_depth[n_aliased++] = b;
      clear_depth_at(b, what);
    }
    const uint32_t z_base = depth_info & 0xFFF;
    const bool z_written = depth_only || (depth_control & 0x4) || (depth_control & 0x1);
    if (z_written && !(c0_written && z_base == c0_base) &&
        std::find(aliased_depth, aliased_depth + n_aliased, z_base) == aliased_depth + n_aliased) {
      clear_depth_at(z_base, what);
      // Depth written over c0's EDRAM (Dead Space 2's light buffer clear: depth-only mode, depth target on 2D0 where
      // the light buffer is): on the console the same EDRAM bytes are the color target's, so the color is cleared by
      // it (depth 0 and stencil 0 there: black). The renderer keeps depth and color in separate images, so the color
      // image is cleared too; without it the lights were added on top of the normals left there by the G-buffer.
      if (z_base == c0_base && (mask & 0xF)) {
        rex::x360::RegistrosCopia c = base;
        c.rb_copy_control = 0u | (1u << 8) | kNullCommand;
        c.rb_color_clear = 0;
        c.rb_color_clear_lo = 0;
        destinos_->Copiar(c);
        what += fmt::format(" c0={:03X} (by the depth over it)", c0_base);
      }
    }
    ++clears_;
    if (clear_shapes_.insert(what).second && clear_shapes_.size() <= 32) {
      REXLOG_INFO("[gpu] clear shape: msaa {}, mode {}:{}", msaa, r[g::XE_GPU_REG_RB_MODECONTROL] & 0x7, what);
    }
    // Single-sampled ones counted apart: they are new, and their game clear color (PS c0) tells whether 0 is right.
    if (msaa ? clears_logged_ < 12 : clears_logged_1x_ < 8) {
      ++(msaa ? clears_logged_ : clears_logged_1x_);
      auto f = [&](uint32_t index) {
        float v;
        std::memcpy(&v, &registers_[index], 4);
        return v;
      };
      REXLOG_INFO("[gpu] clear draw (msaa {}, pitch {}):{}; game had: PS c0 ({:.3f} {:.3f} {:.3f} {:.3f}) VS c0 "
                  "({:.3f} {:.3f} {:.3f} {:.3f}) RB_COLOR_CLEAR {:08X} RB_DEPTH_CLEAR {:08X} depthcontrol {:08X} "
                  "mask {:04X}",
                  msaa, Register(g::XE_GPU_REG_RB_SURFACE_INFO) & 0x3FFF, what, f(0x4400), f(0x4401), f(0x4402),
                  f(0x4403), f(0x4000), f(0x4001), f(0x4002), f(0x4003), Register(g::XE_GPU_REG_RB_COLOR_CLEAR),
                  Register(g::XE_GPU_REG_RB_DEPTH_CLEAR), depth_control, mask);
    }
  }

  // The frame just presented as a 24-bit BMP in capturas/ next to the NRO (ZL + ZR + Down): what is on screen, to look
  // at rendering problems without describing them.
  // Captures every ds2_auto_captura_s and frame dumps on their schedule (seconds since the first Swap).
  void AutomaticCollection() {
    if (!telemetry_) return;
    const auto now = Clock::now();
    if (first_swap_ == Clock::time_point{}) return;
    const double t = std::chrono::duration<double>(now - first_swap_).count();
    const int32_t every = REXCVAR_GET(ds2_auto_captura_s);
    if (every > 0 && t >= next_auto_capture_s_) {
      next_auto_capture_s_ = t + every;
      SaveCapture(true);
    }
    const int32_t max_dumps = REXCVAR_GET(ds2_auto_volcados_max);
    if (auto_dumps_ < uint32_t(std::max(max_dumps, 0))) {
      if (next_auto_dump_s_ < 0) next_auto_dump_s_ = REXCVAR_GET(ds2_auto_volcado_primero_s);
      if (t >= next_auto_dump_s_) {
        ++auto_dumps_;
        const int32_t gap = REXCVAR_GET(ds2_auto_volcado_cada_s);
        next_auto_dump_s_ = gap > 0 ? t + gap : 1e30;
        REXLOG_INFO("[gpu] automatic frame dump {} at {:.0f} s", auto_dumps_, t);
        destinos_->PedirVolcado();
      }
    }
  }

  void SaveCapture(bool small = false) {
    rex::ui::RawImage image;
    if (!presenter_ || !presenter_->CaptureGuestOutput(image) || !image.width || !image.height) {
      REXLOG_WARN("[gpu] capture: no guest output to capture");
      return;
    }
    const auto folder = rex::filesystem::GetExecutableFolder() / "capturas";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    char name[32];
    std::filesystem::path path;
    if (small) {
      std::snprintf(name, sizeof(name), "auto_%04u.bmp", auto_captures_++);
      path = folder / name;
      // Half size: each 2x2 block averaged into the top-left texel, then compacted (0.7 MB instead of 2.7 MB).
      const uint32_t w = image.width / 2, h = image.height / 2;
      std::vector<uint8_t> half(size_t(w) * h * 4);
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
          const uint8_t* a = image.data.data() + size_t(2 * y) * image.stride + size_t(2 * x) * 4;
          const uint8_t* b = a + image.stride;
          for (int k = 0; k < 4; ++k) half[(size_t(y) * w + x) * 4 + k] = uint8_t((a[k] + a[k + 4] + b[k] + b[k + 4] + 2) / 4);
        }
      }
      image.data = std::move(half);
      image.width = w;
      image.height = h;
      image.stride = size_t(w) * 4;
    } else {
      // The first free number once, then counted: the file is written later, so looking for it again found the same
      // free name and captures taken close together overwrote each other.
      if (next_capture_ == UINT32_MAX) {
        for (next_capture_ = 0; next_capture_ < 1000; ++next_capture_) {
          std::snprintf(name, sizeof(name), "cap_%03u.bmp", next_capture_);
          if (!std::filesystem::exists(folder / name, ec)) break;
        }
      }
      std::snprintf(name, sizeof(name), "cap_%03u.bmp", next_capture_++);
      path = folder / name;
    }
    const uint32_t row = image.width * 3;  // 1280 * 3 is a multiple of 4; others are padded below
    const uint32_t padded = (row + 3) & ~3u;
    const uint32_t pixels = padded * image.height;
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(header + at, &v, 4); };
    put32(2, 54 + pixels);
    put32(10, 54);
    put32(14, 40);
    put32(18, image.width);
    put32(22, image.height);
    header[26] = 1;
    header[28] = 24;
    put32(34, pixels);
    std::vector<uint8_t> data(pixels, 0);
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint8_t* s = image.data.data() + size_t(image.height - 1 - y) * image.stride;  // BMP rows go bottom-up
      uint8_t* d = data.data() + size_t(y) * padded;
      for (uint32_t x = 0; x < image.width; ++x, s += 4, d += 3) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
      }
    }
    // One write, on a thread of its own: small writes to the SD card take 10-50 ms each, and the ring thread must not
    // wait for them (the game stalls with it).
    std::vector<uint8_t> file(header, header + sizeof(header));
    file.insert(file.end(), data.begin(), data.end());
    const bool log = !small || auto_captures_ % 12 == 1;  // the automatic ones once a minute in the log
    rex::x360::EncolarEscrituraSd([path, file = std::move(file), log, w = image.width, h = image.height]() {
      if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(file.data(), 1, file.size(), f);
        std::fclose(f);
        if (log) REXLOG_INFO("[gpu] capture: {} ({}x{})", path.string(), w, h);
      } else {
        REXLOG_WARN("[gpu] capture: could not write {}", path.string());
      }
    });
  }

  // One line per draw of a traced light frame: what it draws with and what became of it.
  void DrawTraceLine(bool drawn, const char* why = nullptr) {
    namespace g = rex::graphics;
    if (draw_trace_n_ >= draw_trace_cap_) return;
    const uint32_t init = Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    REXLOG_INFO("[draws] {:3} prim {} n {} vs {} ps {} c0 {:03X}/f{} z {:03X} mask {:04X} blend0 {:08X} colorctl {:08X} "
                "depthctl {:08X} stencil {:08X} mode {} pitch {} msaa {} -> {}",
                draw_trace_n_++, init & 0x3F, init >> 16, vs_ ? int(vs_->numero) : -1, ps_ ? int(ps_->numero) : -1,
                Register(g::XE_GPU_REG_RB_COLOR_INFO) & 0xFFF, (Register(g::XE_GPU_REG_RB_COLOR_INFO) >> 16) & 0xF,
                Register(g::XE_GPU_REG_RB_DEPTH_INFO) & 0xFFF,
                Register(g::XE_GPU_REG_RB_COLOR_MASK), Register(g::XE_GPU_REG_RB_BLENDCONTROL0),
                Register(g::XE_GPU_REG_RB_COLORCONTROL), Register(g::XE_GPU_REG_RB_DEPTHCONTROL),
                Register(g::XE_GPU_REG_RB_STENCILREFMASK), Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7,
                Register(g::XE_GPU_REG_RB_SURFACE_INFO) & 0x3FFF, (Register(g::XE_GPU_REG_RB_SURFACE_INFO) >> 16) & 0x3,
                why ? why : drawn ? "drawn" : "REJECTED by the renderer");
    // The vertex input as D3D patched it: each fetch's format, swizzle, stride and offset. No vertex bytes: guest
    // memory is committed lazily, and even inside a vertex buffer the pages before the draw's first index may be
    // unmapped (reading vertex 0 crashed in game).
    for (size_t i = 0; i + 2 < vs_microcode_.size() && vs_; i += 3) {
      const uint32_t w0 = vs_microcode_[i], w1 = vs_microcode_[i + 1], w2 = vs_microcode_[i + 2];
      if ((w0 & 0x1F) != 0 || !((w0 >> 19) & 1)) continue;
      REXLOG_INFO("[draws]       vfetch r{} const {}.{} format {} swz {:03X} signed {} int {} stride {} offset {}",
                  (w0 >> 12) & 0x3F, (w0 >> 20) & 0x1F, (w0 >> 25) & 3, (w1 >> 16) & 0x3F, w1 & 0xFFF, (w1 >> 12) & 1,
                  (w1 >> 13) & 1, (w2 & 0xFF) * 4, ((w2 >> 8) & 0x7FFFFF) * 4);
    }
    // The pixel shader's textures: fetch constant of each sampler (base, format, size, dimension, swizzle, signs).
    for (size_t i = 0; ps_ && i < ps_->samplers.size() && i < 4; ++i) {
      const uint32_t reg = ps_->samplers[i].registro;
      if (reg >= 32) continue;
      const uint32_t* f = &registers_[0x4800 + reg * 6];
      REXLOG_INFO("[draws]       s{} tipo {}: base {:08X} fmt {} {}x{} dim {} swz {:03X} signs {:02X} tiled {} type {}",
                  reg, ps_->samplers[i].tipo, f[1] & 0xFFFFF000u, f[1] & 0x3F, (f[2] & 0x1FFF) + 1,
                  ((f[2] >> 13) & 0x1FFF) + 1, (f[5] >> 9) & 0x3, (f[3] >> 1) & 0xFFF, (f[0] >> 2) & 0xFF,
                  (f[0] >> 31) & 1, f[0] & 0x3);
    }
  }

  // --- Frame trace -----------------------------------------------------------
  //
  // How the game builds a heavy frame, once a minute up to three times: each pass (a run of draws into the same
  // targets with the same window offset), each resolve and the Swap, one line each. This is what is needed to
  // compare the frame with what the renderer does (multisampling, predicated tiling, EDRAM bases).

  struct FrameState {
    uint64_t draws = 0;              // geometry draws in the frame being counted
    uint64_t predicated_skipped = 0;
  };

  static constexpr uint32_t kTraceMaxLines = 400;

  void FrameTraceSwap() {
    const auto now = Clock::now();
    if (tracing_) {
      FrameTraceClosePass();
      REXLOG_INFO("[frame] end: {} geometry draws, {} predicated packets skipped, {} passes, swap frontbuffer "
                  "fetch0 {:08X} {:08X}",
                  frame_.draws, frame_.predicated_skipped, trace_pass_, Register(0x4800), Register(0x4801));
      tracing_ = false;
      ++traces_done_;
      next_trace_ = now + std::chrono::seconds(std::max(REXCVAR_GET(ds2_traza_cada_s), 1));
    } else if (telemetry_ && traces_done_ < uint32_t(std::max(REXCVAR_GET(ds2_trazas_max), 0)) && frame_.draws >= 800 &&
               now >= next_trace_ && now - first_swap_ >= std::chrono::seconds(60)) {
      tracing_ = true;
      trace_lines_ = 0;
      trace_pass_ = 0;
      trace_key_.fill(0);
      trace_pass_draws_ = 0;
      REXLOG_INFO("[frame] trace {} of a frame ({} draws in the previous one)", traces_done_ + 1, frame_.draws);
    }
    // Per-draw trace of a light frame (loading screens, menus: the interface), with telemetry.
    if (draw_tracing_) {
      draw_tracing_ = false;
      REXLOG_INFO("[draws] end of the traced frame");
    } else if (telemetry_ && draw_traces_ < 8 && frame_.draws >= 10 && frame_.draws <= 400 && now >= next_draw_trace_) {
      draw_tracing_ = true;
      ++draw_traces_;
      draw_trace_n_ = 0;
      draw_trace_cap_ = 400;
      next_draw_trace_ = now + std::chrono::seconds(20);
      REXLOG_INFO("[draws] trace {} of a light frame ({} draws in the previous one)", draw_traces_, frame_.draws);
    }
    if (first_swap_ == Clock::time_point{}) first_swap_ = now;
    frame_ = {};
  }

  void FrameTraceLine(const std::string& line) {
    if (trace_lines_++ < kTraceMaxLines) REXLOG_INFO("[frame] {}", line);
  }

  std::array<uint32_t, 9> FrameTraceKey() const {
    namespace g = rex::graphics;
    return {Register(g::XE_GPU_REG_RB_SURFACE_INFO), Register(g::XE_GPU_REG_RB_COLOR_INFO),
            Register(g::XE_GPU_REG_RB_COLOR1_INFO),  Register(g::XE_GPU_REG_RB_COLOR2_INFO),
            Register(g::XE_GPU_REG_RB_COLOR3_INFO),  Register(g::XE_GPU_REG_RB_DEPTH_INFO),
            Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET), Register(g::XE_GPU_REG_RB_MODECONTROL),
            Register(g::XE_GPU_REG_RB_COLOR_MASK)};
  }

  void FrameTraceClosePass() {
    if (!trace_pass_draws_) return;
    const auto& k = trace_key_;
    std::string colors;
    for (uint32_t i = 0; i < 4; ++i) {
      if ((k[8] >> (i * 4)) & 0xF) {
        colors += fmt::format(" c{}={:03X}/f{}", i, k[1 + i] & 0xFFF, (k[1 + i] >> 16) & 0xF);
      }
    }
    FrameTraceLine(fmt::format(
        "pass {}: {} draws ({} unknown VS), pitch {} msaa {}{} z={:03X}/f{} window ({},{}) mode {} "
        "scissor {:08X}-{:08X} vport x {:.1f}+{:.1f} y {:.1f}+{:.1f} bins {:X}/{:X}",
        trace_pass_, trace_pass_draws_, trace_pass_no_vs_, k[0] & 0x3FFF, (k[0] >> 16) & 0x3, colors, k[5] & 0xFFF,
        (k[5] >> 16) & 0x1, int32_t(k[6] << 17) >> 17, int32_t((k[6] >> 16) << 17) >> 17, k[7] & 0x7,
        trace_scissor_tl_, trace_scissor_br_, trace_vport_[0], trace_vport_[1], trace_vport_[2], trace_vport_[3],
        bin_select_, bin_mask_));
    ++trace_pass_;
    trace_pass_draws_ = 0;
    trace_pass_no_vs_ = 0;
  }

  void FrameTraceDraw() {
    ++frame_.draws;
    if (!tracing_) return;
    const auto key = FrameTraceKey();
    if (key != trace_key_ || !trace_pass_draws_) {
      FrameTraceClosePass();
      trace_key_ = key;
      namespace g = rex::graphics;
      trace_scissor_tl_ = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
      trace_scissor_br_ = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
      for (uint32_t i = 0; i < 4; ++i) {
        // PA_CL_VPORT_XSCALE, XOFFSET, YSCALE, YOFFSET
        const uint32_t v = Register(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + i);
        std::memcpy(&trace_vport_[i], &v, 4);
      }
    }
    ++trace_pass_draws_;
    trace_pass_no_vs_ += vs_ == nullptr;
  }

  // Every distinct resolve, once, when it is first seen (up to 200): the effects (kinesis, hits, screen blood) show up
  // with the time they start, to compare with what is on screen.
  void ResolveCatalog() {
    namespace g = rex::graphics;
    if (resolve_seen_.size() >= 200) return;
    const uint32_t control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
    const uint32_t source = control & 0x7;
    static constexpr uint32_t kColorInfo[4] = {g::XE_GPU_REG_RB_COLOR_INFO, g::XE_GPU_REG_RB_COLOR1_INFO,
                                               g::XE_GPU_REG_RB_COLOR2_INFO, g::XE_GPU_REG_RB_COLOR3_INFO};
    const uint32_t src_info = source < 4 ? Register(kColorInfo[source]) : Register(g::XE_GPU_REG_RB_DEPTH_INFO);
    const uint32_t surface = Register(g::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t tl = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL), br = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    const uint32_t dest = Register(g::XE_GPU_REG_RB_COPY_DEST_BASE), pitch = Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    const uint32_t info = Register(g::XE_GPU_REG_RB_COPY_DEST_INFO), offset = Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    // The rectangle D3D puts in fetch constant 0 (three vertices), as the renderer reads it.
    const uint32_t f0 = Register(0x4800), f1 = Register(0x4801);
    float rect[6] = {};
    if ((f0 & 0x3) == 3 && ((f1 >> 2) & 0xFFFFFF) == 6) {
      const uint8_t* v = memory_->TranslatePhysical(f0 & ~uint32_t(0x3));
      for (int i = 0; i < 6; ++i) {
        uint32_t w;
        std::memcpy(&w, v + i * 4, 4);
        w = xenos::GpuSwap(w, static_cast<xenos::Endian>(f1 & 0x3));
        std::memcpy(&rect[i], &w, 4);
      }
    }
    const uint64_t key = XXH3_64bits(std::array<uint32_t, 10>{control, src_info, surface, tl, br, dest, pitch, info,
                                                              offset, uint32_t(rect[0]) ^ uint32_t(rect[1]) << 16}
                                         .data(),
                                     40);
    if (!resolve_seen_.insert(key).second) return;
    REXLOG_INFO("[resolve] new: {} base {:03X} f{} pitch {} msaa {} scissor {:08X}-{:08X} window ({},{}) rect "
                "({:.0f},{:.0f})-({:.0f},{:.0f})-({:.0f},{:.0f}) -> {:08X} pitch {} height {} fmt {} swap {} control "
                "{:08X}",
                source < 4 ? fmt::format("c{}", source) : std::string("depth"), src_info & 0xFFF,
                (src_info >> 16) & 0xF, surface & 0x3FFF, (surface >> 16) & 0x3, tl, br, int32_t(offset << 17) >> 17,
                int32_t((offset >> 16) << 17) >> 17, rect[0], rect[1], rect[2], rect[3], rect[4], rect[5], dest,
                pitch & 0x3FFF, (pitch >> 16) & 0x3FFF, (info >> 7) & 0x3F, (info >> 24) & 0x1, control);
  }

  void FrameTraceCopy() {
    if (!tracing_) return;
    FrameTraceClosePass();
    namespace g = rex::graphics;
    const uint32_t control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
    const uint32_t source = control & 0x7;
    const uint32_t surface = Register(g::XE_GPU_REG_RB_SURFACE_INFO);
    static constexpr uint32_t kColorInfo[4] = {g::XE_GPU_REG_RB_COLOR_INFO, g::XE_GPU_REG_RB_COLOR1_INFO,
                                               g::XE_GPU_REG_RB_COLOR2_INFO, g::XE_GPU_REG_RB_COLOR3_INFO};
    const uint32_t src_info = source < 4 ? Register(kColorInfo[source]) : Register(g::XE_GPU_REG_RB_DEPTH_INFO);
    const uint32_t dest_info = Register(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    const uint32_t dest_pitch = Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    const uint32_t offset = Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    FrameTraceLine(fmt::format(
        "resolve from {} base {:03X} f{} pitch {} msaa {} rect {:08X}-{:08X} window ({},{}) -> {:08X} pitch {} "
        "height {} fmt {} control {:08X} (copy {}, clear color {} depth {})",
        source < 4 ? fmt::format("c{}", source) : std::string("depth"), src_info & 0xFFF, (src_info >> 16) & 0xF,
        surface & 0x3FFF, (surface >> 16) & 0x3, Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL),
        Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR), int32_t(offset << 17) >> 17,
        int32_t((offset >> 16) << 17) >> 17, Register(g::XE_GPU_REG_RB_COPY_DEST_BASE), dest_pitch & 0x3FFF,
        (dest_pitch >> 16) & 0x3FFF, (dest_info >> 7) & 0x3F, control, (control >> 20) & 0x3, (control >> 8) & 0x1,
        (control >> 9) & 0x1));
  }
#endif

  // EVENT_WRITE_ZPD, as the SDK's CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD: D3D marks the
  // structure with 0xFFFFFEED on Issue(END). With ds2_oclusion_real the renderer counts on the GPU the samples drawn
  // between begin and end (sdk-patches/0017, as ds3-nx); the game is answered at once, never kept waiting for the GPU,
  // with the count the same structure got last time (a frame or two old; the fixed "visible" count the first time).
  // The suspect for the black final fight (the room stops being drawn, a square around ghost Nicole stays): the game
  // measures how much of her is on screen, and always "visible" would keep its hallucination at the maximum.
  void OcclusionQuery(uint32_t address) {
    if (!address) {
      return;
    }
    const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEEDu);  // written big-endian, the struct is LE
    auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
    const bool end_via_z_pass = counts->ZPass_A == kQueryFinished || counts->ZPass_B == kQueryFinished;
    const bool end_via_z_fail = counts->ZFail_A == kQueryFinished || counts->ZFail_B == kQueryFinished;
    std::memset(counts, 0, sizeof(*counts));
    const uint32_t query = address & ~UINT32_C(0x3F);
    const bool real = destinos_ && REXCVAR_GET(ds2_oclusion_real);
    if (real) PollQueries();
    if (!end_via_z_pass && !end_via_z_fail) {
      if (real) destinos_->EmpezarOclusion(query);
      return;
    }
    uint32_t n = kFakeSampleCount;
    if (real) {
      if (const uint64_t ticket = destinos_->TerminarOclusionAsincrona(query)) {
        pending_queries_.push_back({query, ticket});
        if (pending_queries_.size() > 4096) pending_queries_.pop_front();
      }
      if (const auto it = query_counts_.find(query); it != query_counts_.end()) n = it->second;
    }
    counts->ZPass_A = n;
    counts->Total_A = n;
  }

  // The GPU's counts that came back, in order, kept by structure for the next time the game ends that query.
  void PollQueries() {
    while (!pending_queries_.empty()) {
      uint64_t samples = 0;
      if (!destinos_->ResultadoOclusion(pending_queries_.front().ticket, samples)) {
        break;  // the later ones are in later work
      }
      query_counts_[pending_queries_.front().query] = uint32_t(std::min<uint64_t>(samples, INT32_MAX));
      pending_queries_.pop_front();
    }
    if (query_counts_.size() > 8192) query_counts_.clear();
  }

  struct PendingQuery {
    uint32_t query;   // the 64-byte structure
    uint64_t ticket;  // the renderer's
  };
  std::deque<PendingQuery> pending_queries_;               // ended, the GPU has not counted them yet
  std::unordered_map<uint32_t, uint32_t> query_counts_;  // by structure: the last count the GPU gave

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::GraphicsProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  std::unique_ptr<rex::x360::DestinosNativos> destinos_;  // the universal deko renderer backend
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  bool mmio_registered_ = false;
  std::atomic<bool> active_{false};

  std::vector<uint32_t> registers_;
  uint64_t bin_mask_ = ~uint64_t(0);
  uint64_t bin_select_ = ~uint64_t(0);

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_data_{0};
  std::atomic<uint32_t> ring_base_{0};
  std::atomic<uint32_t> ring_words_{0};
  std::atomic<uint32_t> ring_generation_{0};
  std::atomic<uint32_t> write_pointer_{0};
  std::atomic<uint32_t> read_pointer_writeback_{0};
  std::atomic<uint32_t> counter_{0};  // vblanks + swaps, for EVENT_WRITE_SHD
  std::atomic<uint64_t> vblanks_{0};
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_;
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;

  // Only the ring thread touches these.
#if DS2_DEKO3D
  std::vector<uint32_t> microcode_;     // the last IM_LOAD, host byte order
  std::vector<uint32_t> vs_microcode_;  // the last VS load, as D3D patched it
  const rex::x360::EntradaShader* vs_ = nullptr;
  const rex::x360::EntradaShader* ps_ = nullptr;
  std::unordered_map<uint64_t, uint64_t> vs_generations_;
  uint64_t generacion_vs_ = 0, generacion_constantes_vs_ = 0, generacion_constantes_ps_ = 0;
  uint64_t generacion_fetch_ = 0, generacion_encuadre_ = 0;
  uint64_t geometria_ = 0, enviados_ = 0, sin_vs_ = 0, sin_ps_ = 0;
  std::array<uint64_t, 16> color_formats_{};
  std::array<uint64_t, 2> depth_formats_{};
  std::array<uint64_t, 32> primitive_types_{};
  std::unordered_set<uint64_t> resolve_seen_;  // ResolveCatalog
  std::unordered_set<std::string> clear_shapes_;  // EmulateClear
  uint32_t clear_rect_ = 0;          // guest buffer with the emulated clears' rectangle
  double next_auto_capture_s_ = 10, next_auto_dump_s_ = -1;  // AutomaticCollection
  uint32_t auto_captures_ = 0, auto_dumps_ = 0;
  uint64_t depth_greater_ = 0, depth_less_ = 0, clears_ = 0, clears_logged_ = 0, msaa_draws_ = 0;
  FrameState frame_;
  bool telemetry_ = false;
  bool hide_glow_ = false;  // sin_resplandor.flag
  std::array<uint64_t, 8> index_orders_{};
  uint32_t vs_physical_ = 0;
  uint32_t syncs_reported_ = 0;
  uint32_t skip_mode_ = 0, capture_after_skip_ = 0, skip_resume_ = 0;
  uint64_t skipped_by_mode_ = 0;
  uint64_t clears_logged_1x_ = 0;
  std::unordered_set<uint64_t> glow_logged_;
  std::unordered_set<uint64_t> unknown_vs_logged_;
  uint32_t glow_draws_logged_ = 0;
  bool draw_tracing_ = false;
  uint32_t draw_traces_ = 0, draw_trace_n_ = 0, draw_trace_cap_ = 400;
  uint32_t next_capture_ = UINT32_MAX;  // SaveCapture
  Clock::time_point next_draw_trace_{};
  bool tracing_ = false;
  uint32_t traces_done_ = 0, trace_lines_ = 0, trace_pass_ = 0;
  uint64_t trace_pass_draws_ = 0, trace_pass_no_vs_ = 0;
  std::array<uint32_t, 9> trace_key_{};
  uint32_t trace_scissor_tl_ = 0, trace_scissor_br_ = 0;
  float trace_vport_[4] = {};
  Clock::time_point next_trace_{}, first_swap_{};
#endif
  uint64_t swaps_ = 0, draws_ = 0, packets_ = 0, waits_cut_short_ = 0;
  uint64_t reg_mem_waits_ = 0, reg_mem_wait_ns_ = 0;  // WAIT_REG_MEM that had to wait, and how long in total
  uint64_t reported_swaps_ = 0, reported_draws_ = 0, reported_packets_ = 0;
  Clock::time_point last_report_;
};

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem() {
  return std::make_unique<RingGraphicsSystem>();
}

void PonerRampaGamma(const uint8_t* rampa_be) {
  std::lock_guard<std::mutex> cerrojo(g_rampa_cerrojo);
  for (uint32_t c = 0; c < 3; ++c) {
    for (uint32_t i = 0; i < 256; ++i) {
      const uint8_t* v = rampa_be + (c * 256 + i) * 2;
      g_rampa[i][c] = uint16_t(((uint16_t(v[0]) << 8) | v[1]) >> 6);
    }
  }
  g_rampa_nueva = true;
}

}  // namespace ds2::gpu
