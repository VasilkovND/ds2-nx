// nfsmw - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/system/kernel_state.h>  // VIGILANTE DE CUELGUES
#include <rex/system/xthread.h>       // VIGILANTE DE CUELGUES

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>  // HANG WATCHDOG - the signature is sorted by thread id
#include <string>
#include <thread>
#include <vector>

#if defined(NFSMW_NATIVE_SHADER_LIBRARY)
#include "nfsmw_shader_hooks.h"
#endif
#include "nfsmw_ajustes_graficos.h"
#include "nfsmw_entorno_mesa.h"
#include "nfsmw_nativo_captura.h"
#include "nfsmw_nativo_sistema.h"
#include "nfsmw_perfil_pc.h"
#include "nfsmw_prueba_entrada.h"
#include "nfsmw_video_nativo.h"  // HANG WATCHDOG - cutscene frames with FFmpeg

class NfsmwApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NfsmwApp>(new NfsmwApp(ctx, "nfsmw",
        PPCImageConfig));
  }

  // ==========================================================================
  //  0. NATIVE RENDERER
  //
  //  With nfsmw_renderizador = "nativo" the graphics system is the app's own
  //  (nfsmw_nativo_sistema.cpp) and the emulation plugin is not loaded:
  //  ReXApp::SetupPresentation only loads it if config.graphics is empty.
  //  The code default is still emulation. See docs/native-renderer.md.
  // ==========================================================================
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (nfsmw::nativo::Activo()) {
      config.graphics = nfsmw::nativo::CrearSistemaGrafico();
    }
    // Automated tests: virtual gamepad if nfsmw_prueba_botones has a script.
    nfsmw::prueba::EnvolverEntrada(config);
  }

  // Available hooks, unused:
  //   void OnLoadXexImage(std::string& xex_image) override {}
  //   void OnPostLoadXexImage() override {}
  //   void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  //   void OnShutdown() override {}
  //
  // The three below are used: portable paths, mandatory settings
  // and the fps counter.

 protected:
  // ==========================================================================
  //  1. PORTABLE PATHS: find the ISO next to the .exe
  //
  //  Without this, starting without --game_data_root dies with
  //      "--game_data_root was not provided."
  //  because SetupEnvironment only looks at the cvar and, if it is empty,
  //  ConstructRuntime aborts.
  //
  //  OnConfigurePaths is called right after the PathConfig is built and
  //  before anyone uses it, so it is the place to fill the gap.
  //
  //  ORDER MATTERS: this runs before nfsmw.toml is loaded (the SDK
  //  reads it a few lines further down, in SetupEnvironment). So the actual
  //  priority is: --game_data_root from the command line, and otherwise
  //  whatever is found right here. Setting game_data_root in the toml does
  //  not work, and that is not our doing: it is how the SDK is ordered.
  //
  //  It searches, in this order:
  //    1. an .iso whose name matches the executable's
  //    2. any other .iso in the folder, in alphabetical order
  //    3. a game_root\ folder, in case someone prefers to extract it
  //
  //  (1) exists so that a folder with NFS_Most_Wanted.exe and
  //  NFS_Most_Wanted.iso works unambiguously even if there are more images.
  // ==========================================================================
  // ==========================================================================
  //  COPY OF THE SAVES, NEXT TO THE EXECUTABLE
  //
  //  The game's saves live where the runtime puts them, which is an opaque
  //  place: <data>/<xuid>/<title>/<type>/<name>. The player cannot see them and
  //  there is no convenient way to copy them or recover a corrupted one.
  //
  //  With content_backup_root, the runtime also leaves a copy sorted by
  //  profile when each save is closed:
  //      saves/<profile>/actual      the latest save
  //      saves/<profile>/anterior    the previous one, in case the latest gets corrupted
  //
  //  On the Switch the NRO is in sdmc:/switch/nfsmw/, so this gives
  //  sdmc:/switch/nfsmw/saves. On PC, next to the .exe, like everything else.
  //
  //  ORDER: this runs before nfsmw.toml is read, so the toml can
  //  change the folder or leave it empty to copy nothing.
  // ==========================================================================
  void ElegirCarpetaDeGuardados() {
    if (rex::cvar::GetFlagInfo("content_backup_root") == nullptr) {
      return;  // SDK without that option: nothing happens
    }
    if (!rex::cvar::GetFlagByName("content_backup_root").empty()) {
      return;  // given on the command line
    }
    const auto carpeta = rex::filesystem::GetExecutableFolder();
    if (carpeta.empty()) {
      return;
    }
    const auto destino = carpeta / "saves";
    if (!rex::cvar::SetFlagByName("content_backup_root", rex::path_to_utf8(destino))) {
      REXLOG_WARN("[guardado] no se pudo fijar la carpeta de copias en {}",
                  rex::path_to_utf8(destino));
    }
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    ElegirCarpetaDeGuardados();
    if (!paths.game_data_root.empty()) {
      return;  // given on the command line; it takes precedence.
    }

    std::error_code ec;
    const auto carpeta = rex::filesystem::GetExecutableFolder();
    if (carpeta.empty() || !std::filesystem::is_directory(carpeta, ec)) {
      return;
    }

    // The executable's name, for the preferred case.
    std::filesystem::path preferida;
    std::vector<std::filesystem::path> otras;

    std::string yo;
    {
      const auto exe = rex::filesystem::GetExecutablePath();
      if (!exe.empty()) {
        yo = exe.stem().string();
        std::transform(yo.begin(), yo.end(), yo.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
      }
    }

    for (const auto& e : std::filesystem::directory_iterator(carpeta, ec)) {
      if (ec) break;
      if (!e.is_regular_file(ec)) continue;

      std::string ext = e.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (ext != ".iso") continue;

      std::string base = e.path().stem().string();
      std::transform(base.begin(), base.end(), base.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });

      if (!yo.empty() && base == yo) {
        preferida = e.path();
      } else {
        otras.push_back(e.path());
      }
    }

    if (!preferida.empty()) {
      paths.game_data_root = preferida;
    } else if (!otras.empty()) {
      std::sort(otras.begin(), otras.end());
      paths.game_data_root = otras.front();
    } else {
      // No ISO: an extracted folder next to it also works. The ISO patch
      // left --game_data_root accepting both.
      const auto extraida = carpeta / "game_root";
      if (std::filesystem::is_directory(extraida, ec)) {
        paths.game_data_root = extraida;
      }
    }
    // If nothing is found, it is left empty on purpose: the SDK gives its
    // own message, which is clearer than anything we could put here.
  }

  // ==========================================================================
  //  2. MANDATORY SETTINGS
  //
  //  So that plain "NFS_Most_Wanted.exe", without a single argument, starts
  //  just as well as with the usual long command line.
  //
  //  Only the settings not set explicitly are touched: HasNonDefaultValue
  //  tells "this is the factory default" from "someone asked for this". That
  //  way the command line and nfsmw.toml still take precedence.
  //
  //  WHY IN TWO DIFFERENT PLACES
  //  The readback_resolve cvar does not exist yet when logging starts: it is
  //  registered by the GPU plugin (rexgpu-xenos.dll), which is loaded later, in
  //  SetupPresentation. Setting it earlier would mean writing to a flag that
  //  does not exist yet. Hence:
  //
  //    OnPostInitLogging  -> gpu_plugin and mnk_mode, which belong to the runtime
  //                          and are already registered. And it has to be here,
  //                          because SetupPresentation reads gpu_plugin right after.
  //    OnPostSetup        -> readback_resolve, once the plugin has loaded and
  //                          before a single frame has been drawn.
  // ==========================================================================
  void OnPostInitLogging() override {
    // Mesa/NVK environment variables (nfsmw_mesa_entorno): the toml has already been read and Vulkan is
    // created later, in SetupPresentation.
    nfsmw::entorno::AplicarEntornoMesa();
    // Internal resolution and FPS limit that work with the native renderer: passed to the video mode before
    // the game requests it, and the settings that do nothing are removed from the F4 menu.
    nfsmw::ajustes::AplicarAjustesGraficos();
    nfsmw::ajustes::OcultarAjustesSinEfecto();
    // Optional post-processing (Graficos/Posproceso) and antialiasing: the output pass picks them up live.
    nfsmw::ajustes::VigilarAjustesEnVivo();
    // Without a GPU plugin the screen stays black: the game runs, but the
    // runtime discards its graphics calls with "no GPU emulation loaded".
    PonerSiNadieLoPidio("gpu_plugin", "xenos");
#if defined(NFSMW_NATIVE_SHADER_LIBRARY)
    // VkDevice must be created with these capabilities; enabling them after
    // SetupPresentation does not change a device that already exists.
    PonerSiNadieLoPidio("vulkan_native_shader_features", "true");
#endif
#if !REX_PLATFORM_SWITCH
    // Keyboard and mouse in addition to the gamepad. The Switch has neither:
    // the synthetic device would only shadow the libnx controllers.
    PonerSiNadieLoPidio("mnk_mode", "true");
#endif
  }

  void OnPostSetup() override {
#if defined(NFSMW_NATIVE_SHADER_LIBRARY)
    nfsmw::native::IniciarBibliotecaShaders();
#endif
    // This is not a preference, it is a fix. The game computes its exposure
    // by measuring the average brightness of the scene and reading that value
    // back on the CPU. That readback is disabled by default ("none"), so the
    // game receives garbage, concludes that the scene is extremely dark and
    // raises the exposure to the maximum: washed-out image and a blown-out sun.
    PonerSiNadieLoPidio("readback_resolve", "fast");

    // FPS counter for the F3 overlay, see below.
    SetGuestFrameStats([this] { return MuestreaFotograma(); });

    // Hang watchdog, see below.
    ArrancarVigilante();

    // PNG captures for the native renderer tests (nfsmw_captura_cada_s;
    // off by default). It works the same with emulation, for comparison.
    nfsmw::captura::Arrancar([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* grafico = rt ? rt->graphics_system() : nullptr;
      return grafico ? grafico->presenter() : nullptr;
    });

    // Sampling CPU profiler, PC only (nfsmw_perfil_pc_desde_s; off by default).
    nfsmw::perfil_pc::Arrancar();
  }

  void OnShutdown() override {
    nfsmw::perfil_pc::Parar();
    nfsmw::captura::Parar();
    PararVigilante();
  }

 private:
  static void PonerSiNadieLoPidio(const char* nombre, const char* valor) {
    if (rex::cvar::GetFlagInfo(nombre) == nullptr) {
      REXLOG_DEBUG("Ajuste '{}' no registrado todavia; no lo toco.", nombre);
      return;
    }
    if (rex::cvar::HasNonDefaultValue(nombre)) {
      return;  // set explicitly: do not override it.
    }
    if (rex::cvar::SetFlagByName(nombre, valor)) {
      REXLOG_DEBUG("Ajuste por defecto de la build portable: {} = {}", nombre, valor);
    }
  }

  // ==========================================================================
  //  3. FPS COUNTER FOR THE F3 OVERLAY
  //
  //  In a Release build, F3 opens an empty box that only says "Debug". There
  //  are two separate causes and both were closed doors:
  //
  //    1. Almost the whole panel lives inside #ifdef REXGLUE_ENABLE_PERF_COUNTERS,
  //       and the SDK's CMakeLists says
  //         add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
  //       so in Release the define is not applied. That is on purpose:
  //       "compiled out in Release", says its comment.
  //
  //    2. The "Guest: X FPS" line is not inside that #ifdef. It only needs
  //       someone to register a provider with SetGuestFrameStats, and nothing in
  //       the SDK calls it: it is an API the app has to use.
  //
  //  (2) is the door that can be opened without touching the SDK.
  //
  //  WHAT IT MEASURES: the game's frames, counted in the
  //  Swap hook (g_nfsmw_fotogramas_juego, nfsmw_d3d_trace.cpp). An earlier
  //  version measured how often the overlay drew, and with a menu open the UI
  //  thread repaints nonstop: it showed ~60 FPS with the game at 4 or stopped.
  //
  //  The provider is called by the overlay's OnDraw on every repaint. It is
  //  recomputed at most once per second from the frames of that window, and
  //  smoothed a little (averaged with the previous window) so it is readable.
  // ==========================================================================
  rex::ui::FrameStats MuestreaFotograma() {
    extern std::atomic<uint64_t> g_nfsmw_fotogramas_juego;
    using Reloj = std::chrono::steady_clock;
    const auto ahora = Reloj::now();
    const uint64_t total = g_nfsmw_fotogramas_juego.load(std::memory_order_relaxed);
    stats_.frame_count = total > 0 ? total : 1;  // the overlay does not draw if this is 0

    if (!tiene_anterior_) {
      tiene_anterior_ = true;
      ultimo_ = ahora;
      fotogramas_ = total;
      return stats_;
    }
    const double dt_ms =
        std::chrono::duration<double, std::milli>(ahora - ultimo_).count();
    if (dt_ms < 1000.0) {
      return stats_;
    }
    const uint64_t hechos = total - fotogramas_;
    ultimo_ = ahora;
    fotogramas_ = total;

    if (hechos == 0) {
      // The game has not presented anything in the whole window.
      suave_ms_ = 0.0;
      stats_.fps = 0.0;
      stats_.frame_time_ms = dt_ms;
      return stats_;
    }
    const double ms_por_fotograma = dt_ms / double(hechos);
    // After a long time with the overlay closed, the previous window is useless.
    suave_ms_ = (suave_ms_ <= 0.0 || dt_ms > 5000.0) ? ms_por_fotograma
                                                     : (suave_ms_ + ms_por_fotograma) * 0.5;
    stats_.frame_time_ms = suave_ms_;
    stats_.fps = 1000.0 / suave_ms_;
    return stats_;
  }

  // ==========================================================================
  //  4. HANG WATCHDOG
  //
  //  THE PROBLEM IT SOLVES
  //  When returning to the menu the game freezes, and absolutely nothing
  //  shows up in the log: no error, no kernel call, no graphics command.
  //  Total silence until the window is closed. That rules out an exception
  //  or an unregistered function (those are visible) and leaves a single
  //  explanation: all the game's threads are stopped at once, waiting for
  //  something that never arrives.
  //
  //  And a deadlock cannot be diagnosed from the log, because what defines
  //  it is precisely that nothing gets written anymore. The threads have to
  //  be asked directly.
  //
  //  HOW IT WORKS, AND WHY IT NEEDS NO NOTIFICATION
  //  A separate thread looks at all guest threads once per second and records
  //  two registers of each one:
  //
  //    lr  where the current function would return to. It changes constantly
  //        in code that makes progress.
  //    r1  the stack pointer. Same.
  //
  //  If for several seconds in a row no thread has moved either of the two,
  //  the game is not slow: it is stopped. Then the table is dumped.
  //
  //  The advantage of measuring it this way is that it depends on nothing:
  //  not on the frame counter (which only runs with the overlay open), not on
  //  the game calling the kernel, not on the graphics thread staying alive.
  //  If everything stops, it shows precisely because everything stops.
  //
  //  WHAT THE DUMP GIVES
  //  For each thread: its entry address (which tells which thread it is), lr,
  //  r1 and r13. With that, a thread that waits (lr stuck in a kernel wait
  //  function) can be told apart from one that spins (lr jumping between two
  //  or three addresses). And since it is dumped every 15 seconds while it
  //  lasts, it shows whether something moves very slowly or not at all.
  //
  //  COST WHEN NOTHING HAPPENS
  //  One pass per second reading two integers per thread. Negligible.
  //
  //  It lives in the app and not in the SDK on purpose: that way it can be
  //  changed without rebuilding the whole SDK, and it does not impose a
  //  watchdog thread on anyone else.
  // ==========================================================================

  void ArrancarVigilante() {
    vigilante_activo_ = true;
    vigilante_ = std::thread([this] { VigilanteMain(); });
  }

  void PararVigilante() {
    vigilante_activo_ = false;
    if (vigilante_.joinable()) {
      vigilante_.join();
    }
  }

  // Dump of the thread table. 'grave' decides whether it goes out as an error (when
  // it is a real alarm) or as debug (the routine snapshots).
  template <typename Lista>
  static void VolcarHilos(const Lista& hilos, bool grave) {
    for (auto& h : hilos) {
      const auto* cp = h->creation_params();
      auto* estado = h->thread_state();
      if (estado && estado->context()) {
        const auto& c = *estado->context();
        if (grave) {
          REXLOG_ERROR("[vigilante]   hilo id=0x{:X} entrada=0x{:08X} principal={} corriendo={} | "
                       "lr=0x{:08X} r1=0x{:08X} r13=0x{:08X} r3=0x{:08X} ctr=0x{:08X} "
                       "ultimo_indirecto=0x{:08X}",
                       h->thread_id(), cp->start_address, h->main_thread(), h->is_running(),
                       static_cast<uint32_t>(c.lr), c.r1.u32, c.r13.u32, c.r3.u32, c.ctr.u32,
                       c.last_indirect_target);
        } else {
          REXLOG_DEBUG("[vigilante]   hilo id=0x{:X} entrada=0x{:08X} principal={} corriendo={} | "
                       "lr=0x{:08X} r1=0x{:08X} r13=0x{:08X} r3=0x{:08X} ctr=0x{:08X} "
                       "ultimo_indirecto=0x{:08X}",
                       h->thread_id(), cp->start_address, h->main_thread(), h->is_running(),
                       static_cast<uint32_t>(c.lr), c.r1.u32, c.r13.u32, c.r3.u32, c.ctr.u32,
                       c.last_indirect_target);
        }
      } else {
        REXLOG_DEBUG("[vigilante]   hilo id=0x{:X} entrada=0x{:08X} sin contexto", h->thread_id(),
                     cp->start_address);
      }
    }
  }

  void VigilanteMain() {
    using Reloj = std::chrono::steady_clock;

    // How many seconds in a row without anything moving before raising the
    // alarm. Five is generous: this game at 10 fps still moves registers
    // a hundred times per second, so five still seconds are not slowness.
    constexpr int kSegundosParaSospechar = 5;
    constexpr int kSegundosEntreVolcados = 15;

    uint64_t firma_anterior = 0;
    int quietos = 0;
    int desde_ultimo_volcado = 0;
    int desde_instantanea = 0;
    bool avisado = false;
    int volcados_pilas = 0;  // logs/pilas_N.txt on PC (nfsmw_perfil_pc.cpp)

    while (vigilante_activo_) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!vigilante_activo_) break;

      auto* kernel = rex::system::kernel_state();
      if (!kernel) continue;

      auto hilos = kernel->object_table()->GetObjectsByType<rex::system::XThread>();
      if (hilos.empty()) continue;

      // A signature of "where everyone is". It does not need to be a
      // good hash: it only has to change if some register changes.
      //
      // Watch the order. The first version of this multiplied and mixed
      // on the fly, walking the list as it came. And GetObjectsByType does
      // not guarantee the order: in the real dumps the threads came out
      // shuffled from one round to the next, and even repeated (0x6 appeared
      // twice). So the signature changed on its own even when nothing moved,
      // and the alarm never fired in the real hang. The only thing that
      // helped at all was the periodic snapshots further down.
      //
      // The fix is to put each thread in a map keyed by its id: the map sorts
      // itself, so the shuffling no longer matters, and a repeated id
      // overwrites instead of being counted twice. Only then is it mixed.
      std::map<uint32_t, uint64_t> por_hilo;
      for (auto& h : hilos) {
        auto* estado = h->thread_state();
        if (!estado || !estado->context()) continue;
        const auto& c = *estado->context();
        por_hilo[h->thread_id()] =
            static_cast<uint64_t>(c.lr) ^ (static_cast<uint64_t>(c.r1.u32) << 20) ^
            (static_cast<uint64_t>(c.r3.u32) << 40);
      }

      uint64_t firma = 1469598103934665603ull;
      for (const auto& [id_hilo, huella] : por_hilo) {
        firma = (firma ^ id_hilo) * 1099511628211ull;
        firma = (firma ^ huella) * 1099511628211ull;
      }
      // Cutscenes with FFmpeg (nfsmw_video_nativo.cpp): the video player threads wait almost all the time
      // in the same place and the signature may not change even though the video advances. On PC the alarm
      // fired during attract_movie with the video at 30 frames/s and the audio without gaps.
      firma = (firma ^ nfsmw::video_nativo::FotogramasNativos()) * 1099511628211ull;
      // Native renderer swaps. With the blocking waits (nfsmw_espera_anillo.cpp and
      // nfsmw_espera_fotograma.cpp) the main thread and the D3D thread sleep almost the whole frame in the
      // same place, and the signature may not change even though the game keeps drawing: the alarm fired
      // in the menus of a PC race test with the PM4 ring thread drawing (118,332 draws in those 10 s).
      firma = (firma ^ nfsmw::nativo::SwapsNativos()) * 1099511628211ull;

      // PERIODIC SNAPSHOT, WHATEVER HAPPENS.
      //
      // The alarm above only fires if nothing moves, and it turned out that
      // the hang being chased is not of that kind: the registers kept
      // changing, meaning the game runs code but makes no progress. A tight
      // loop waiting for something that never arrives looks just as stuck
      // from outside, and yet the alarm does not catch it.
      //
      // That is what this is for: every ten seconds it records where each
      // thread is, whether there is a problem or not. When the game freezes,
      // two or three snapshots of the bad stretch remain, and if lr cycles
      // between the same two or three addresses, there is the loop.
      //
      // It logs at debug level (no noise in normal use) and it is a few
      // lines every ten seconds.
      if (++desde_instantanea >= 10) {
        desde_instantanea = 0;
        REXLOG_DEBUG("[vigilante] instantanea: {} hilos del juego", hilos.size());
        VolcarHilos(hilos, false);
      }

      if (firma != firma_anterior) {
        if (avisado) {
          REXLOG_WARN("[vigilante] el juego ha vuelto a moverse despues de {} s parado.", quietos);
          avisado = false;
        }
        firma_anterior = firma;
        quietos = 0;
        desde_ultimo_volcado = 0;
        continue;
      }

      ++quietos;
      ++desde_ultimo_volcado;
      if (quietos < kSegundosParaSospechar) continue;
      if (avisado && desde_ultimo_volcado < kSegundosEntreVolcados) continue;
      desde_ultimo_volcado = 0;

      REXLOG_ERROR("[vigilante] {} s sin que se mueva ni un registro en ninguno de los {} hilos "
                   "del juego. Esto no es lentitud: esta parado.",
                   quietos, hilos.size());
      VolcarHilos(hilos, true);
      // And the stacks of every thread in the process, host ones included (ring, audio, copies): on PC it
      // writes logs/pilas_N.txt (nfsmw_perfil_pc.cpp). Only on the first two alarms.
      if (volcados_pilas < 2) {
        ++volcados_pilas;
        nfsmw::perfil_pc::VolcarPilas("vigilante");
      }
      avisado = true;
    }
  }

  rex::ui::FrameStats stats_{};
  std::chrono::steady_clock::time_point ultimo_{};
  double suave_ms_ = 0.0;
  uint64_t fotogramas_ = 0;
  bool tiene_anterior_ = false;

  std::thread vigilante_;
  std::atomic<bool> vigilante_activo_{false};
};
