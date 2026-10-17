// nfsmw - sampling CPU profiler and stack dumper for the PC build (see nfsmw_perfil_pc.h).
//
// Careful with the pause (as in switch_perf.cpp): while a thread is suspended, nothing may be done that could
// take a lock that thread holds (no allocation, no stdio, no logger). Between SuspendThread and ResumeThread
// there are only system calls; samples go to a vector whose capacity was reserved beforehand.

#include "nfsmw_perfil_pc.h"

#if defined(_WIN32)

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#define PSAPI_VERSION 2  // K32EnumProcessModules and friends, in kernel32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_INT32(nfsmw_perfil_pc_desde_s, 0, "NFSMW",
                     "PC: muestrear la CPU de los hilos ocupados desde este segundo de vida del proceso "
                     "(0 = nunca; solo pruebas). Deja logs/perfil_pc.csv")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_perfil_pc_duracion_s, 20, "NFSMW",
                     "PC: segundos de muestreo de nfsmw_perfil_pc_desde_s")
    .range(1, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_perfil_pc_pilas_s, 0, "NFSMW",
                     "PC: volcar las pilas de todos los hilos en este segundo de vida del proceso (0 = nunca; "
                     "solo pruebas). Deja logs/pilas_N.txt; el vigilante tambien vuelca al ver el juego parado")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::perfil_pc {
namespace {

constexpr size_t kCandidatos = 6;     // stack addresses that fall inside a known module
constexpr size_t kPalabrasPila = 64;  // how many stack words are inspected per sample

struct Modulo {
  uint64_t base = 0;
  uint64_t tam = 0;
  std::string nombre;
  std::string ruta;
};

struct Hilo {
  DWORD id = 0;
  HANDLE h = nullptr;
  std::string nombre;
  uint64_t tiempo_prev = 0;  // 100 ns de CPU (usuario + nucleo)
  bool ocupado = false;
  uint64_t muestras = 0;
  double cpu_max = 0.0;
};

struct Muestra {
  uint32_t hilo = 0;
  uint64_t pc = 0;
  uint64_t cand[kCandidatos] = {};
};

std::atomic<bool> g_parar{false};
std::thread g_hilo;
std::thread g_hilo_pilas;
std::mutex g_volcado_mutex;
int g_volcados = 0;  // con g_volcado_mutex

std::string Utf8(const wchar_t* w) {
  if (!w || !*w) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) {
    return {};
  }
  std::string s(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string NombreHilo(HANDLE h) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const Fn leer =
      reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!leer) {
    return {};
  }
  PWSTR desc = nullptr;
  std::string nombre;
  if (SUCCEEDED(leer(h, &desc)) && desc) {
    nombre = Utf8(desc);
    LocalFree(desc);
  }
  return nombre;
}

double SegundosDeVida() {
  FILETIME creacion{}, salida{}, nucleo{}, usuario{};
  if (!GetProcessTimes(GetCurrentProcess(), &creacion, &salida, &nucleo, &usuario)) {
    return 0.0;
  }
  FILETIME ahora{};
  GetSystemTimeAsFileTime(&ahora);
  const uint64_t c = (uint64_t(creacion.dwHighDateTime) << 32) | creacion.dwLowDateTime;
  const uint64_t a = (uint64_t(ahora.dwHighDateTime) << 32) | ahora.dwLowDateTime;
  return a > c ? double(a - c) / 1e7 : 0.0;
}

std::vector<Modulo> ListarModulos() {
  std::vector<Modulo> modulos;
  HMODULE lista[1024];
  DWORD bytes = 0;
  if (!EnumProcessModules(GetCurrentProcess(), lista, sizeof(lista), &bytes)) {
    return modulos;
  }
  for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i) {
    MODULEINFO info{};
    char nombre[MAX_PATH] = {};
    char ruta[MAX_PATH * 2] = {};
    if (!GetModuleInformation(GetCurrentProcess(), lista[i], &info, sizeof(info))) {
      continue;
    }
    GetModuleBaseNameA(GetCurrentProcess(), lista[i], nombre, sizeof(nombre));
    GetModuleFileNameExA(GetCurrentProcess(), lista[i], ruta, sizeof(ruta));
    modulos.push_back({uint64_t(uintptr_t(info.lpBaseOfDll)), uint64_t(info.SizeOfImage), nombre, ruta});
  }
  std::sort(modulos.begin(), modulos.end(), [](const Modulo& a, const Modulo& b) { return a.base < b.base; });
  return modulos;
}

// Index of the module that contains the address, or -1.
int ModuloDe(const std::vector<Modulo>& modulos, uint64_t direccion) {
  auto it = std::upper_bound(modulos.begin(), modulos.end(), direccion,
                             [](uint64_t d, const Modulo& m) { return d < m.base; });
  if (it == modulos.begin()) {
    return -1;
  }
  --it;
  return direccion - it->base < it->tam ? int(it - modulos.begin()) : -1;
}

std::vector<Hilo> ListarHilos() {
  std::vector<Hilo> hilos;
  const HANDLE foto = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (foto == INVALID_HANDLE_VALUE) {
    return hilos;
  }
  THREADENTRY32 e{};
  e.dwSize = sizeof(e);
  const DWORD proceso = GetCurrentProcessId();
  const DWORD yo = GetCurrentThreadId();
  for (BOOL ok = Thread32First(foto, &e); ok; ok = Thread32Next(foto, &e)) {
    if (e.th32OwnerProcessID != proceso || e.th32ThreadID == yo) {
      continue;
    }
    const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                FALSE, e.th32ThreadID);
    if (!h) {
      continue;
    }
    Hilo hilo;
    hilo.id = e.th32ThreadID;
    hilo.h = h;
    hilo.nombre = NombreHilo(h);
    hilos.push_back(std::move(hilo));
  }
  CloseHandle(foto);
  return hilos;
}

uint64_t TiempoCpu(HANDLE h) {
  FILETIME c{}, s{}, n{}, u{};
  if (!GetThreadTimes(h, &c, &s, &n, &u)) {
    return 0;
  }
  return ((uint64_t(n.dwHighDateTime) << 32) | n.dwLowDateTime) +
         ((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime);
}

void Bucle(int desde_s, int duracion_s) {
  while (!g_parar.load() && SegundosDeVida() < double(desde_s)) {
    Sleep(200);
  }
  if (g_parar.load()) {
    return;
  }
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  HANDLE temporizador = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
  const std::vector<Modulo> modulos = ListarModulos();
  std::vector<Hilo> hilos = ListarHilos();
  std::vector<Muestra> muestras;
  muestras.reserve(size_t(duracion_s) * 1000 * 8);

  const double inicio = SegundosDeVida();
  LARGE_INTEGER frecuencia{}, t0{}, t{};
  QueryPerformanceFrequency(&frecuencia);
  QueryPerformanceCounter(&t0);
  LONGLONG ultima_revision = t0.QuadPart - 2 * frecuencia.QuadPart;
  const LONGLONG fin = t0.QuadPart + LONGLONG(duracion_s) * frecuencia.QuadPart;
  uint64_t fallos_pausa = 0;
  uint64_t perdidas = 0;

  for (;;) {
    QueryPerformanceCounter(&t);
    if (g_parar.load() || t.QuadPart >= fin) {
      break;
    }
    // Every second: which threads are busy (more than 5 % of a core since the previous check).
    if (t.QuadPart - ultima_revision >= frecuencia.QuadPart) {
      const double segundos = double(t.QuadPart - ultima_revision) / double(frecuencia.QuadPart);
      for (Hilo& h : hilos) {
        const uint64_t cpu = TiempoCpu(h.h);
        const double fraccion = h.tiempo_prev && segundos < 10.0
                                    ? double(cpu - h.tiempo_prev) / 1e7 / segundos
                                    : 0.0;
        h.ocupado = fraccion > 0.05;
        h.cpu_max = std::max(h.cpu_max, fraccion);
        h.tiempo_prev = cpu;
      }
      ultima_revision = t.QuadPart;
    }
    for (size_t i = 0; i < hilos.size(); ++i) {
      Hilo& h = hilos[i];
      if (!h.ocupado) {
        continue;
      }
      if (muestras.size() == muestras.capacity()) {
        ++perdidas;  // no growth: no allocation inside the loop
        continue;
      }
      if (SuspendThread(h.h) == DWORD(-1)) {
        ++fallos_pausa;
        continue;
      }
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      const bool ok = GetThreadContext(h.h, &ctx) != 0;
      uint64_t pila[kPalabrasPila] = {};
      SIZE_T leidos = 0;
      if (ok) {
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(uintptr_t(ctx.Rsp)), pila,
                          sizeof(pila), &leidos);
      }
      ResumeThread(h.h);
      if (!ok) {
        continue;
      }
      Muestra m;
      m.hilo = uint32_t(i);
      m.pc = ctx.Rip;
      size_t n = 0;
      for (size_t k = 0; k < leidos / sizeof(uint64_t) && n < kCandidatos; ++k) {
        if (ModuloDe(modulos, pila[k]) >= 0) {
          m.cand[n++] = pila[k];
        }
      }
      muestras.push_back(m);
      ++h.muestras;
    }
    if (temporizador) {
      LARGE_INTEGER plazo{};
      plazo.QuadPart = -10000;  // 1 ms, relativo
      SetWaitableTimer(temporizador, &plazo, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(temporizador, 20);
    } else {
      Sleep(1);
    }
  }
  const double fin_s = SegundosDeVida();
  if (temporizador) {
    CloseHandle(temporizador);
  }

  // Everything that allocates or writes comes after sampling.
  const std::filesystem::path ruta = rex::filesystem::GetExecutableFolder() / "logs" / "perfil_pc.csv";
  std::error_code ec;
  std::filesystem::create_directories(ruta.parent_path(), ec);
  std::FILE* f = _wfopen(ruta.c_str(), L"wb");
  if (f) {
    std::fprintf(f, "#muestreo;%.1f;%.1f;%zu\n", inicio, fin_s, muestras.size());
    for (size_t i = 0; i < modulos.size(); ++i) {
      std::fprintf(f, "#modulo;%zu;%s;%llX;%llX\n", i, modulos[i].nombre.c_str(),
                   static_cast<unsigned long long>(modulos[i].base),
                   static_cast<unsigned long long>(modulos[i].tam));
    }
    for (size_t i = 0; i < hilos.size(); ++i) {
      if (hilos[i].muestras) {
        std::fprintf(f, "#hilo;%zu;%lu;%s;%llu;%.3f\n", i, static_cast<unsigned long>(hilos[i].id),
                     hilos[i].nombre.c_str(), static_cast<unsigned long long>(hilos[i].muestras),
                     hilos[i].cpu_max);
      }
    }
    const auto escribir = [&](uint64_t d) {
      const int mod = ModuloDe(modulos, d);
      if (mod < 0) {
        std::fprintf(f, ";-;%llX", static_cast<unsigned long long>(d));
      } else {
        std::fprintf(f, ";%d;%llX", mod, static_cast<unsigned long long>(d - modulos[size_t(mod)].base));
      }
    };
    for (const Muestra& m : muestras) {
      std::fprintf(f, "%u", m.hilo);
      escribir(m.pc);
      for (size_t k = 0; k < kCandidatos && m.cand[k]; ++k) {
        escribir(m.cand[k]);
      }
      std::fputc('\n', f);
    }
    std::fclose(f);
  }
  REXLOG_INFO("[perfil_pc] {} muestras en {:.1f} s (de {:.1f} a {:.1f} s de vida), {} pausas fallidas, {} "
              "perdidas; {}",
              muestras.size(), fin_s - inicio, inicio, fin_s, fallos_pausa, perdidas,
              f ? ruta.string() : std::string("no se pudo escribir el fichero"));
  for (const Hilo& h : hilos) {
    if (h.muestras) {
      REXLOG_INFO("[perfil_pc] hilo {} \"{}\": {} muestras, CPU maxima {:.0f} %", h.id, h.nombre, h.muestras,
                  h.cpu_max * 100.0);
    }
  }
  for (Hilo& h : hilos) {
    CloseHandle(h.h);
  }
}

// --- Stack dump ------------------------------------------------------------------------------------------------
// For hangs (the ring thread can stop while the profiler above only sees busy threads). It suspends threads
// one at a time, copies their context and the top of their stack, and resumes them; with nobody suspended
// any more, it unwinds each copy with its module's unwind table (.pdata) and RtlVirtualUnwind, with the
// registers that point into the stack relocated to the copy. A blocked thread does not move, so its copy is
// its real stack; that of a thread that was running may come out truncated.

constexpr size_t kBytesPila = 256 * 1024;
constexpr size_t kMargenPila = 32 * 1024;  // RtlVirtualUnwind reads a bit above the frame it unwinds
constexpr size_t kMarcos = 48;

struct PilaCopiada {
  CONTEXT ctx{};
  std::vector<uint8_t> datos;
  size_t bytes = 0;
  bool ok = false;
};

// The module's .pdata entry for that address, read from the loaded image (without the locks of
// RtlLookupFunctionEntry, in case some thread held them).
const RUNTIME_FUNCTION* EntradaDesenrollado(const Modulo& m, uint64_t pc) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(uintptr_t(m.base));
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return nullptr;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(uintptr_t(m.base + uint64_t(dos->e_lfanew)));
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    return nullptr;
  }
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (!dir.VirtualAddress || dir.Size < sizeof(RUNTIME_FUNCTION)) {
    return nullptr;
  }
  const auto* tabla = reinterpret_cast<const RUNTIME_FUNCTION*>(uintptr_t(m.base + dir.VirtualAddress));
  const uint32_t rva = uint32_t(pc - m.base);
  size_t lo = 0;
  size_t hi = dir.Size / sizeof(RUNTIME_FUNCTION);
  while (lo < hi) {
    const size_t mitad = lo + (hi - lo) / 2;
    if (rva < tabla[mitad].BeginAddress) {
      hi = mitad;
    } else if (rva >= tabla[mitad].EndAddress) {
      lo = mitad + 1;
    } else {
      const RUNTIME_FUNCTION* f = &tabla[mitad];
      if (f->UnwindData & 1) {  // indirect entry: points to another RUNTIME_FUNCTION
        f = reinterpret_cast<const RUNTIME_FUNCTION*>(uintptr_t(m.base + (f->UnwindData & ~DWORD(1))));
      }
      return f;
    }
  }
  return nullptr;
}

// One protected RtlVirtualUnwind step: an odd table or stack does not bring the game down. No objects with
// destructors (__try does not allow them).
bool PasoDesenrollado(uint64_t base, const RUNTIME_FUNCTION* f, CONTEXT* ctx) {
  __try {
    PVOID datos_manejador = nullptr;
    DWORD64 marco = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, const_cast<PRUNTIME_FUNCTION>(f), ctx,
                     &datos_manejador, &marco, nullptr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::vector<uint64_t> Desenrollar(const PilaCopiada& p, const std::vector<Modulo>& modulos) {
  std::vector<uint64_t> marcos;
  if (!p.ok) {
    return marcos;
  }
  CONTEXT ctx = p.ctx;
  const uint64_t original = p.ctx.Rsp;
  const uint64_t copia = uint64_t(uintptr_t(p.datos.data()));
  const uint64_t bytes = p.bytes;
  const auto en_copia = [&](uint64_t d, uint64_t tam) { return d >= copia && d + tam <= copia + bytes; };
  // In UNWIND_INFO register number order (0 = RAX ... 15 = R15).
  const std::array<DWORD64*, 16> registros = {&ctx.Rax, &ctx.Rcx, &ctx.Rdx, &ctx.Rbx, &ctx.Rsp, &ctx.Rbp,
                                              &ctx.Rsi, &ctx.Rdi, &ctx.R8,  &ctx.R9,  &ctx.R10, &ctx.R11,
                                              &ctx.R12, &ctx.R13, &ctx.R14, &ctx.R15};
  const auto trasladar = [&] {
    for (DWORD64* r : registros) {
      if (*r >= original && *r < original + bytes) {
        *r = *r - original + copia;
      }
    }
  };
  trasladar();
  marcos.push_back(ctx.Rip);
  for (size_t k = 1; k < kMarcos && en_copia(ctx.Rsp, 8); ++k) {
    const int mod = ModuloDe(modulos, ctx.Rip);
    if (mod < 0) {
      break;
    }
    const Modulo& m = modulos[size_t(mod)];
    const RUNTIME_FUNCTION* f = EntradaDesenrollado(m, ctx.Rip);
    if (!f) {
      if (k != 1) {
        break;  // only the top frame can be a leaf function without .pdata
      }
      std::memcpy(&ctx.Rip, reinterpret_cast<const void*>(uintptr_t(ctx.Rsp)), 8);
      ctx.Rsp += 8;
    } else {
      const auto* info = reinterpret_cast<const uint8_t*>(uintptr_t(m.base + f->UnwindData));
      const uint8_t registro_marco = info[3] & 0x0F;
      const bool pasado_prologo = uint32_t(ctx.Rip - m.base) - f->BeginAddress >= info[1];
      if (registro_marco && pasado_prologo && !en_copia(*registros[registro_marco], 1)) {
        break;  // the frame is based on a register that does not point into the copy
      }
      if (!PasoDesenrollado(m.base, f, &ctx)) {
        break;
      }
    }
    if (!ctx.Rip) {
      break;
    }
    marcos.push_back(ctx.Rip);
    trasladar();  // non-volatile registers restored from the stack hold values from the original stack
  }
  return marcos;
}

}  // namespace

void VolcarPilas(const char* motivo) {
  std::lock_guard<std::mutex> cerrojo(g_volcado_mutex);
  const int numero = ++g_volcados;
  const std::vector<Modulo> modulos = ListarModulos();
  std::vector<Hilo> hilos = ListarHilos();
  std::vector<PilaCopiada> pilas(hilos.size());
  for (PilaCopiada& p : pilas) {
    p.datos.assign(kBytesPila + kMargenPila, 0);
  }
  const double vida = SegundosDeVida();
  for (size_t i = 0; i < hilos.size(); ++i) {
    PilaCopiada& p = pilas[i];
    if (SuspendThread(hilos[i].h) == DWORD(-1)) {
      continue;
    }
    p.ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(hilos[i].h, &p.ctx)) {
      p.ok = true;
      MEMORY_BASIC_INFORMATION region{};
      if (VirtualQuery(reinterpret_cast<LPCVOID>(uintptr_t(p.ctx.Rsp)), &region, sizeof(region))) {
        const uint64_t fin = uint64_t(uintptr_t(region.BaseAddress)) + region.RegionSize;
        const uint64_t pedir = fin > p.ctx.Rsp ? std::min<uint64_t>(fin - p.ctx.Rsp, kBytesPila) : 0;
        SIZE_T leidos = 0;
        if (pedir && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(uintptr_t(p.ctx.Rsp)),
                                       p.datos.data(), SIZE_T(pedir), &leidos)) {
          p.bytes = size_t(leidos);
        }
      }
    }
    ResumeThread(hilos[i].h);
  }

  // Nobody is suspended any more.
  const std::filesystem::path ruta =
      rex::filesystem::GetExecutableFolder() / "logs" / ("pilas_" + std::to_string(numero) + ".txt");
  std::error_code ec;
  std::filesystem::create_directories(ruta.parent_path(), ec);
  std::FILE* f = _wfopen(ruta.c_str(), L"wb");
  if (f) {
    std::fprintf(f, "#volcado;%s;%.1f;%zu\n", motivo, vida, hilos.size());
    for (size_t i = 0; i < modulos.size(); ++i) {
      std::fprintf(f, "#modulo;%zu;%s;%llX;%llX;%s\n", i, modulos[i].nombre.c_str(),
                   static_cast<unsigned long long>(modulos[i].base),
                   static_cast<unsigned long long>(modulos[i].tam), modulos[i].ruta.c_str());
    }
    for (size_t i = 0; i < hilos.size(); ++i) {
      const std::vector<uint64_t> marcos = Desenrollar(pilas[i], modulos);
      const unsigned long id = static_cast<unsigned long>(hilos[i].id);
      std::fprintf(f, "#hilo;%lu;%s;%d;%zu;%zu\n", id, hilos[i].nombre.c_str(), pilas[i].ok ? 1 : 0,
                   pilas[i].bytes, marcos.size());
      for (size_t k = 0; k < marcos.size(); ++k) {
        const int mod = ModuloDe(modulos, marcos[k]);
        if (mod < 0) {
          std::fprintf(f, "%lu;%zu;-;%llX\n", id, k, static_cast<unsigned long long>(marcos[k]));
        } else {
          std::fprintf(f, "%lu;%zu;%d;%llX\n", id, k, mod,
                       static_cast<unsigned long long>(marcos[k] - modulos[size_t(mod)].base));
        }
      }
    }
    std::fclose(f);
  }
  REXLOG_WARN("[perfil_pc] pilas de {} hilos volcadas ({}): {}", hilos.size(), motivo,
              f ? ruta.string() : std::string("no se pudo escribir el fichero"));
  for (Hilo& h : hilos) {
    CloseHandle(h.h);
  }
}

void Arrancar() {
  g_parar.store(false);
  const int pilas_s = REXCVAR_GET(nfsmw_perfil_pc_pilas_s);
  if (pilas_s > 0 && !g_hilo_pilas.joinable()) {
    g_hilo_pilas = std::thread([pilas_s] {
      while (!g_parar.load() && SegundosDeVida() < double(pilas_s)) {
        Sleep(200);
      }
      if (!g_parar.load()) {
        VolcarPilas("nfsmw_perfil_pc_pilas_s");
      }
    });
  }
  const int desde_s = REXCVAR_GET(nfsmw_perfil_pc_desde_s);
  if (desde_s <= 0 || g_hilo.joinable()) {
    return;
  }
  g_hilo = std::thread(Bucle, desde_s, int(REXCVAR_GET(nfsmw_perfil_pc_duracion_s)));
  REXLOG_INFO("[perfil_pc] muestreo de CPU desde {} s de vida del proceso, {} s", desde_s,
              int(REXCVAR_GET(nfsmw_perfil_pc_duracion_s)));
}

void Parar() {
  g_parar.store(true);
  if (g_hilo.joinable()) {
    g_hilo.join();
  }
  if (g_hilo_pilas.joinable()) {
    g_hilo_pilas.join();
  }
}

}  // namespace nfsmw::perfil_pc

#endif  // _WIN32
