// nfsmw - environment variables for Mesa/NVK before creating Vulkan
//
// On Horizon, NVK and the common part of Mesa read their options with getenv when the instance, the physical
// device and the logical device are created, and the NRO did not set any. Useful ones:
//   MESA_SHADER_CACHE_DISABLE=true   no Mesa on-disk shader cache (sdmc:/.mesa). That cache works alongside
//                                    cache/nfsmw_nativo_pipelines.bin: it reads from the SD card on every miss and
//                                    writes each new shader with up to 4 threads of its own with 8 MB stacks.
//   NVK_SWITCH_PERF_LOG=1            counters and timers of the Horizon backend (diagnostics only)
//   NVK_SWITCH_CPU_WRITE_MEM_UNCACHED=0
//                                    NVK commands and transient memory with CPU cache (by default they have none)
//   NOUVEAU_HORIZON_BO_CACHE_MB=N    size of NVK's BO cache (128 by default)
//
// nfsmw_mesa_entorno is a list "VARIABLE=value;VARIABLE=value". It is applied from OnPostInitLogging: the toml
// has already been read and Vulkan is created later, in SetupPresentation. On the Switch it also logs whether
// Mesa's on-disk shader cache exists and how big it is, to know whether it really writes to the SD card.

#include "nfsmw_entorno_mesa.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

/*
 * The ceiling that killed the game.
 *
 * The error behind the black and green screens was 0x235C = LibnxNvidiaError_SharedMemoryTooSmall (Nvidia
 * 0x1000). It was not a lack of memory (that would be InsufficientMemory): the area shared with nvdrv runs
 * out.
 *
 * On Horizon every GPU allocation is wrapped with nvMapCreate, and the bookkeeping for each handle lives in
 * the transfer memory that libnx gives nvdrv at startup. libnx sets it to 8 MB and leaves it as a weak
 * symbol (`__nx_nv_transfermem_size`), so it can be changed; and since it is read when Mesa calls
 * nvInitialize() (that is, after the toml has been loaded), it can be adjusted without rebuilding.
 *
 * The numbers match what was observed: the game died with about 3,600 live textures plus the render targets
 * and buffers. At a couple of KB of bookkeeping per handle, 8 MB is enough for about 4,000.
 *
 * Default 0 = leave it alone, so that nothing changes if the toml does not set it.
 */
REXCVAR_DEFINE_INT32(nfsmw_switch_nvmap_mb, 0, "NFSMW",
                     "Switch: MB del area que libnx comparte con nvdrv, donde vive la contabilidad de cada reserva "
                     "de GPU. libnx usa 8; subirla sube el numero de reservas que caben. 0 = dejar la de libnx")
    .range(0, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * What follows was tested and turned out to be wrong. Reverted. Do not try it again.
 *
 * The change did take effect (the libnx counters in the profiler prove it: `armDCacheClean` went from 2.51
 * to 14.89 MB/s (61.9 sigma), and +12.38 MB/s at 29.1 fps is 0.425 MB per frame = 312 bytes per recorded
 * draw = 78 dwords, which is exactly the push buffer and exactly the size the theory assumed. The path
 * really changed). But the cost did not move:
 *
 *     `grabar`: 3.871 -> 3.820 us per recorded draw, 95% CI [-0.18 ; +0.08], 0.78 sigma.
 *
 * The theory predicted -3 to -4 us. Rejected at more than 40 sigma: it is not "not visible", it "did not
 * happen".
 *
 * WHY IT WAS WRONG: on a Cortex-A57, stores to Normal Non-Cacheable memory go out through the write buffer
 * and are combined; they do not cost ~45 ns each. Those 45 ns are the round trip of Device memory or of a
 * read, not of a posted store. The 78 dwords per draw were never worth 3.5 us. The 3.8 us of `grabar` is
 * NVK encoding logic (a vkCmdDrawIndexed drags the whole flush_gfx_state along), not memory latency.
 *
 * And on top of that it came out net negative: +1.23 ms/s of cache maintenance and `presentar` from 1.328
 * to 1.457 ms per Swap (2.98 sigma), the only counter that really moved in the whole comparison.
 *
 * -------------------- what was believed before, kept so that it is not repeated --------------------
 * The push buffer was written without CPU cache, and that was believed to be the per-draw cost.
 *
 * This variable had been listed in the comment above but was never tested. The reasoning at the time,
 * breaking down the 3.8 us it costs to record a draw in NVK:
 *
 *   - nvk_device.c:288-291 sets cmd_mem_cpu_uncached = true by default on Horizon, and nvk_cmd_pool.c:26-33
 *     translates that into NVKMD_MEM_COHERENT, which there selects an NvMap without CPU cache.
 *   - So every dword the driver writes while recording is an uncached store on a Cortex-A57.
 *   - And it gets worse: nv_push.h:237-271 rewrites the packet header after every dword
 *     (`*push->last_hdr = hdr_dw`), so the pattern is [header][data][header][data]...: it doubles the
 *     stores and breaks write-combining because the addresses are not contiguous.
 *   - Budget per draw: viewport+scissor ~19 dw, bias ~8, blend constants ~5, the draw ~7, and the
 *     descriptor binding ~40 when needed. That is 80-160 uncached stores at ~45 ns = the 3.8 us measured.
 *
 * The cached path is already implemented and is the one normal NVK uses: nvk_cmd_mem_mark_dirty
 * (nvk_cmd_buffer.c:176) records how far it wrote and nvk_cmd_mem_flush (:182) does one
 * nvkmd_mem_sync_map_to_gpu per BO in EndCommandBuffer. Mesa's own comment says the option exists for
 * "cached-CPU comparisons".
 *
 * This is not exclusive to the "grabar" stage (5.25 ms per frame): everything that emits commands goes
 * through there, including registers, pipeline and uploads. That is why it shipped enabled.
 *
 * If there is corruption or a hang: set nfsmw_mesa_entorno = "" in the toml to go back to the previous
 * behavior.
 * ----------------------------------------------------------------------------------------------------
 */
REXCVAR_DEFINE_STRING(nfsmw_mesa_entorno, "", "NFSMW",
                      "Variables de entorno para Mesa/NVK que se ponen antes de crear Vulkan, como "
                      "\"VARIABLE=valor;VARIABLE=valor\" (p. ej. MESA_SHADER_CACHE_DISABLE=true); vacio = ninguna")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * Mesa's on-disk shader cache, turned off.
 *
 * Mesa stored every compiled shader in sdmc:/.mesa (8.7 MB on the console), and those same binaries are
 * already stored by our pipeline cache, which the prewarm uses together with its list (both in
 * cache/nfsmw_nativo_pipelines.bin). It was a second copy on the SD card, with its own writer threads.
 * nfsmw_mesa_entorno is applied afterwards: "MESA_SHADER_CACHE_DISABLE=false" there turns it back on.
 */
REXCVAR_DEFINE_BOOL(nfsmw_mesa_cache_disco, false, "NFSMW",
                    "Cache de shaders en disco de Mesa (sdmc:/.mesa). Duplica cache/nfsmw_nativo_pipelines.bin; false = ni se "
                    "crea ni se usa")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::entorno {
namespace {

std::string Recortar(const std::string& texto) {
  const size_t inicio = texto.find_first_not_of(" \t\r\n");
  if (inicio == std::string::npos) {
    return {};
  }
  const size_t fin = texto.find_last_not_of(" \t\r\n");
  return texto.substr(inicio, fin - inicio + 1);
}

#if REX_PLATFORM_SWITCH
// Mesa's on-disk shader cache: MESA_SHADER_CACHE_DIR, or $HOME/.mesa, or sdmc:/.mesa (disk_cache_os.c).
void AnotarCacheMesa() {
  std::filesystem::path base;
  if (const char* dir = std::getenv("MESA_SHADER_CACHE_DIR"); dir && *dir) {
    base = dir;
  } else if (const char* home = std::getenv("HOME"); home && *home) {
    base = std::filesystem::path(home) / ".mesa";
  } else {
    base = "sdmc:/.mesa";
  }
  const char* desactivada = std::getenv("MESA_SHADER_CACHE_DISABLE");
  const std::string nota =
      desactivada ? std::string(" (MESA_SHADER_CACHE_DISABLE=") + desactivada + ")" : std::string();
  std::error_code error;
  if (!std::filesystem::is_directory(base, error)) {
    REXLOG_INFO("[mesa] cache de shaders en disco: {} no existe{}", base.string(), nota);
    return;
  }
  uint64_t bytes = 0;
  uint32_t ficheros = 0;
  for (auto it = std::filesystem::recursive_directory_iterator(base, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    std::error_code error_fichero;
    if (it->is_regular_file(error_fichero)) {
      const uintmax_t tam = it->file_size(error_fichero);
      if (!error_fichero) {
        bytes += tam;
      }
      ++ficheros;
    }
  }
  REXLOG_INFO("[mesa] cache de shaders en disco: {} con {} ficheros y {} KB{}", base.string(), ficheros, bytes >> 10,
              nota);
}
#endif

}  // namespace

#if REX_PLATFORM_SWITCH
/*
 * libnx's weak symbol. It is declared here because it is not in any public header: it comes from the .data
 * of nv.o, with 0x800000 (8 MB) as its default value, and nm marks it as "V", i.e. a weak object.
 */
extern "C" uint32_t __nx_nv_transfermem_size;

void AjustarAreaDeNvmap() {
  const int32_t mb = REXCVAR_GET(nfsmw_switch_nvmap_mb);
  if (mb <= 0) {
    REXLOG_INFO("[mesa] area de nvmap: se deja la de libnx ({} MB)", __nx_nv_transfermem_size >> 20);
    return;
  }
  const uint32_t antes = __nx_nv_transfermem_size;
  __nx_nv_transfermem_size = uint32_t(mb) << 20;
  REXLOG_INFO("[mesa] area de nvmap: {} MB -> {} MB (contabilidad de las reservas de GPU; si el juego no arranca, "
              "poner nfsmw_switch_nvmap_mb = 0)",
              antes >> 20, __nx_nv_transfermem_size >> 20);
}
#endif  // REX_PLATFORM_SWITCH

void AplicarEntornoMesa() {
#if REX_PLATFORM_SWITCH
  AjustarAreaDeNvmap();
#endif
  if (!REXCVAR_GET(nfsmw_mesa_cache_disco)) {
#if defined(_WIN32)
    const int error = _putenv_s("MESA_SHADER_CACHE_DISABLE", "true");
#else
    const int error = setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
#endif
    REXLOG_INFO("[mesa] cache de shaders en disco de Mesa: apagada (nfsmw_mesa_cache_disco = false){}; los shaders "
                "compilados se guardan solo en cache/nfsmw_nativo_pipelines.bin",
                error ? " (no se pudo poner la variable)" : "");
  }
  const std::string lista = REXCVAR_GET(nfsmw_mesa_entorno);
  size_t inicio = 0;
  while (inicio <= lista.size()) {
    size_t fin = lista.find(';', inicio);
    if (fin == std::string::npos) {
      fin = lista.size();
    }
    const std::string par = Recortar(lista.substr(inicio, fin - inicio));
    inicio = fin + 1;
    if (par.empty()) {
      continue;
    }
    const size_t igual = par.find('=');
    if (igual == std::string::npos || igual == 0) {
      REXLOG_WARN("[mesa] entorno: '{}' no es VARIABLE=valor; se ignora", par);
      continue;
    }
    const std::string nombre = Recortar(par.substr(0, igual));
    const std::string valor = Recortar(par.substr(igual + 1));
#if defined(_WIN32)
    const int error = _putenv_s(nombre.c_str(), valor.c_str());
#else
    const int error = setenv(nombre.c_str(), valor.c_str(), 1);
#endif
    REXLOG_INFO("[mesa] entorno: {}={}{}", nombre, valor, error ? " (no se pudo poner)" : "");
  }
#if REX_PLATFORM_SWITCH
  AnotarCacheMesa();
#endif
}

}  // namespace nfsmw::entorno
