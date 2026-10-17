// nfsmw - profile dump of the instrumented build for PGO.
//
// Only compiled with NFSMW_PGO=generar (CMakeLists.txt). For each object, GCC embeds in the executable the
// path NFSMW_PGO_DIR\CMakeFiles#...#fichero.gcda (NFSMW_PGO_DIR is the profile folder pgo/<edition> as a
// Windows path on the PC that compiles). That cannot be opened on the Switch, so the fopen --wrap redirects
// it to sdmc:/switch/nfsmw/pgo/CMakeFiles#...#fichero.gcda.
//
// The Switch does not always close the program cleanly (the HOME menu kills it), so the profile cannot be
// left for exit: a thread dumps it every 3 minutes and resets the counters. libgcov adds to whatever each
// .gcda already holds, so several dumps give the total for the run.
#if defined(NFSMW_PGO_GENERAR) && defined(__SWITCH__)

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

extern "C" {
FILE* __real_fopen(const char* ruta, const char* modo);
void __gcov_dump(void);
void __gcov_reset(void);

FILE* __wrap_fopen(const char* ruta, const char* modo) {
  static constexpr char kPrefijo[] = NFSMW_PGO_DIR;
  if (ruta && std::strncmp(ruta, kPrefijo, sizeof(kPrefijo) - 1) == 0) {
    const char* resto = ruta + sizeof(kPrefijo) - 1;
    while (*resto == '\\' || *resto == '/') {
      ++resto;
    }
    std::string nueva = "sdmc:/switch/nfsmw/pgo/";
    for (; *resto; ++resto) {
      nueva += (*resto == '\\') ? '/' : *resto;
    }
    return __real_fopen(nueva.c_str(), modo);
  }
  return __real_fopen(ruta, modo);
}
}

namespace {
Thread g_hilo_pgo;

void HiloPgo(void*) {
  for (int volcado = 1;; ++volcado) {
    svcSleepThread(180LL * 1000 * 1000 * 1000);
    mkdir("sdmc:/switch/nfsmw/pgo", 0777);
    const u64 antes = armGetSystemTick();
    __gcov_dump();
    __gcov_reset();
    const double ms = double(armTicksToNs(armGetSystemTick() - antes)) / 1e6;
    if (FILE* f = __real_fopen("sdmc:/switch/nfsmw/pgo/volcados.txt", "a")) {
      std::fprintf(f, "volcado %d: %.0f ms\n", volcado, ms);
      std::fclose(f);
    }
  }
}

__attribute__((constructor)) void IniciarPgo() {
  // The game's normal priority (0x3B) on any core: it only works once every 3 minutes.
  if (R_SUCCEEDED(threadCreate(&g_hilo_pgo, HiloPgo, nullptr, nullptr, 64 * 1024, 0x3B, -2))) {
    threadStart(&g_hilo_pgo);
  }
}
}  // namespace

#endif
