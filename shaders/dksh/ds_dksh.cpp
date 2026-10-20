/**
 * @file    ds_dksh.cpp
 * @brief   The deko3d shader library to a DKSH cache, on the PC.
 *
 * Each (shader, specialization value) goes the way the console would take it: SPIR-V of the library -> GLSL
 * (rex::deko3d::SpirvAGlsl, SPIRV-Cross) -> DKSH (uam with SwitchWakerHD's patches). The result is the cache file
 * the renderer loads (rex/ui/deko3d/cache_dksh.h), and a report: what failed and why, every line uam wrote, and
 * the compile times. Each variant compiles in its own child process with a time limit, on every core.
 *
 * Variants: every vertex shader with the usual value (with all its interpolators and with the position only), and
 * every pixel shader with each value that changes its code: the 8 alpha test functions, the cheap PCF, the shadow
 * minimum, no blur, and without 1/size (the texture size query).
 *
 * Usage
 *   ds_dksh <ds2_shaders.dslib> <output cache> [--contenedores DIR] [--glsl DIR] [--informe FILE.tsv]
 *           [--hilos N] [--limite SECONDS]
 *   --contenedores   the library's container folder: the report uses its names instead of numbers.
 *   Exit code 0 only if every variant compiled.
 */

#include "../XenosRecomp/shader_common.h"
#include <rex/ui/deko3d/cache_dksh.h>
#include <rex/ui/deko3d/glsl.h>
#include "../ds_shader_library.h"
#include "uam_api.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
// Inline, with the memcpy reads of ds_shader_library.cpp (see the note there).
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> Leer(const fs::path& ruta) {
  std::ifstream f(ruta, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() < 0) throw std::runtime_error("no se puede leer " + ruta.string());
  std::vector<uint8_t> d(static_cast<size_t>(f.tellg()));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(d.data()), d.size())) throw std::runtime_error("lectura incompleta: " + ruta.string());
  return d;
}

void Escribir(const fs::path& ruta, const void* datos, size_t bytes) {
  std::ofstream f(ruta, std::ios::binary);
  if (!f.write(static_cast<const char*>(datos), bytes) || !f.flush())
    throw std::runtime_error("no se puede escribir " + ruta.string());
}

// The value nearly every pipeline has: constants through UBOs (nothing else in deko3d) and 1/size of the textures.
constexpr uint32_t kBase = SPEC_CONSTANT_CONSTANTES_UBO | SPEC_CONSTANT_INV_TAMANO_TEX;

// One variant to compile: a library shader, its specialization value and, for a vertex shader, the interpolators
// it keeps (SpirvAGlsl's `salidas`).
struct Variante {
  size_t shader = 0;
  uint32_t especializacion = 0;
  uint32_t salidas = ~0u;
};

std::vector<uint32_t> EspecializacionesPorDefecto(bool vertices) {
  if (vertices) return {kBase};
  std::vector<uint32_t> v = {kBase, 0};
  for (uint32_t f = 0; f < 8; ++f)
    v.push_back(kBase | SPEC_CONSTANT_ALPHA_TEST | (f << SPEC_CONSTANT_ALPHA_FUNC_SHIFT));
  v.push_back(kBase | SPEC_CONSTANT_PCF_BARATO);
  v.push_back(kBase | SPEC_CONSTANT_PCF_BARATO | SPEC_CONSTANT_SOMBRA_MINIMO);
  v.push_back(kBase | SPEC_CONSTANT_SIN_DESENFOQUE);
  return v;
}

// How a variant ended: compiled, rejected (conversion or uam), out of time, or its process died.
enum class Resultado { kOk, kFallo, kTiempo, kSenal };
const char* NombreResultado(Resultado r) {
  switch (r) {
    case Resultado::kOk: return "ok";
    case Resultado::kFallo: return "fallo";
    case Resultado::kTiempo: return "tiempo";
    case Resultado::kSenal: return "senal";
  }
  return "?";
}

struct Fila {
  std::string shader;
  uint32_t especializacion = 0;
  uint32_t variante = 0;
  Resultado resultado = Resultado::kFallo;
  double ms = 0;
  size_t bytes = 0;
  uint32_t texturas = 0, ubos = 0;
  std::string motivo;  // conversion error, or uam's log
};

std::string UnaLinea(std::string s) {
  for (char& c : s)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

// One variant through the console's path: SPIR-V -> GLSL -> DKSH. Runs in a child process (POSIX) or inline.
struct Compilado {
  bool ok = false;
  uint32_t texturas = 0, ubos = 0, avisos_conversion = 0;
  std::string motivo;  // conversion error and warnings, then uam's log on one line
  std::string log;     // uam's log as it came, for the count by kind
  std::vector<uint8_t> dksh;
};

Compilado Compilar(const std::vector<uint32_t>& spirv, uint32_t especializacion, uint32_t salidas,
                   const fs::path& archivo_glsl) {
  Compilado c;
  const rex::deko3d::GlslDeko g = rex::deko3d::SpirvAGlsl(spirv, especializacion, salidas);
  if (g.glsl.empty()) {
    c.motivo = "conversion: " + g.error;
    return c;
  }
  c.texturas = g.texturas;
  c.ubos = g.ubos;
  for (const std::string& a : g.avisos) {
    ++c.avisos_conversion;
    c.motivo += "[conversion] " + a + " ";
  }
  if (!archivo_glsl.empty()) Escribir(archivo_glsl, g.glsl.data(), g.glsl.size());
  uam::Result r = uam::compile(g.vertices ? uam::Stage::Vertex : uam::Stage::Fragment, g.glsl.c_str());
  c.ok = r.ok;
  c.motivo += UnaLinea(r.log);
  c.log = std::move(r.log);
  if (r.ok) c.dksh = std::move(r.dksh);
  return c;
}

// The child's answer through its pipe: four u32 (ok, textures, UBOs, conversion warnings), then motivo, log and
// the DKSH, each as a u32 length and its bytes.
void PonerU32(std::string& d, uint32_t v) { d.append(reinterpret_cast<const char*>(&v), 4); }
void PonerBytes(std::string& d, const void* p, size_t n) {
  PonerU32(d, uint32_t(n));
  d.append(static_cast<const char*>(p), n);
}
std::string Serializar(const Compilado& c) {
  std::string d;
  PonerU32(d, c.ok);
  PonerU32(d, c.texturas);
  PonerU32(d, c.ubos);
  PonerU32(d, c.avisos_conversion);
  PonerBytes(d, c.motivo.data(), c.motivo.size());
  PonerBytes(d, c.log.data(), c.log.size());
  PonerBytes(d, c.dksh.data(), c.dksh.size());
  return d;
}
bool Deserializar(const std::string& d, Compilado& c) {
  size_t at = 0;
  auto u32 = [&](uint32_t& v) {
    if (d.size() - at < 4) return false;
    std::memcpy(&v, d.data() + at, 4);
    at += 4;
    return true;
  };
  auto bytes = [&](auto& destino) {
    uint32_t n = 0;
    if (!u32(n) || d.size() - at < n) return false;
    destino.assign(d.data() + at, d.data() + at + n);
    at += n;
    return true;
  };
  uint32_t ok = 0;
  if (!u32(ok) || !u32(c.texturas) || !u32(c.ubos) || !u32(c.avisos_conversion)) return false;
  c.ok = ok != 0;
  return bytes(c.motivo) && bytes(c.log) && bytes(c.dksh) && at == d.size();
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc < 3) {
    std::fprintf(stderr,
                 "uso: ds_dksh <ds2_shaders.dslib> <cache de salida> [--contenedores DIR]"
                 " [--glsl DIR] [--informe FICHERO.tsv] [--hilos N] [--limite SEGUNDOS]\n");
    return 1;
  }
  const fs::path biblioteca_ruta = argv[1], salida = argv[2];
  fs::path contenedores, carpeta_glsl, informe;
  // Processes at once (default: every core) and the time one variant may take before it is killed.
  unsigned hilos = std::max(1u, std::thread::hardware_concurrency());
  double limite_s = 120;
  for (int i = 3; i + 1 < argc; i += 2) {
    const std::string o = argv[i];
    if (o == "--contenedores") contenedores = argv[i + 1];
    else if (o == "--glsl") carpeta_glsl = argv[i + 1];
    else if (o == "--informe") informe = argv[i + 1];
    else if (o == "--hilos") hilos = std::max(1, std::atoi(argv[i + 1]));
    else if (o == "--limite") limite_s = std::max(1.0, std::atof(argv[i + 1]));
    else throw std::runtime_error("opcion desconocida: " + o);
  }

  ds::BibliotecaShaders biblioteca;
  biblioteca.Cargar(biblioteca_ruta);
  const auto& shaders = biblioteca.shaders();
  if (shaders.empty()) throw std::runtime_error("la biblioteca no tiene shaders");

  // Names for the report: the first container (in name order) with each fingerprint.
  std::map<uint64_t, std::string> nombres;
  if (!contenedores.empty()) {
    std::vector<fs::path> rutas;
    for (const auto& e : fs::directory_iterator(contenedores))
      if (e.path().extension() == ".bin") rutas.push_back(e.path());
    std::sort(rutas.begin(), rutas.end());
    for (const auto& r : rutas) {
      const auto d = Leer(r);
      nombres.emplace(XXH3_64bits(d.data(), d.size()), r.stem().string());
    }
  }
  auto nombre = [&](size_t i) {
    const auto it = nombres.find(shaders[i].huella);
    return it != nombres.end() ? it->second : (shaders[i].vertices ? "vs" : "ps") + std::to_string(i);
  };

  // Every vertex shader with all its interpolators and with the position only, which checks the removal of unread
  // outputs.
  std::vector<Variante> trabajo;
  for (size_t i = 0; i < shaders.size(); ++i)
    for (uint32_t e : EspecializacionesPorDefecto(shaders[i].vertices)) {
      trabajo.push_back({i, e, ~0u});
      if (shaders[i].vertices) trabajo.push_back({i, e, 0});
    }
  if (!carpeta_glsl.empty()) fs::create_directories(carpeta_glsl);

  uam::init(true);
  rex::deko3d::CacheDksh cache;
  std::vector<Fila> filas;
  std::map<std::string, size_t> avisos_uam;
  size_t avisos_conversion = 0;
  // The key is the value masked to the bits the shader reads (MascaraEspecializacion): variants that differ only
  // in other bits are the same DKSH, compiled once.
  std::vector<uint32_t> mascaras(shaders.size());
  size_t mascaras_erroneas = 0;
  for (size_t i = 0; i < shaders.size(); ++i) {
    mascaras[i] = rex::deko3d::MascaraEspecializacion(shaders[i].spirv);
    // The same from the GLSL: every line that names the constant, as SPIRV-Cross writes it. A bit read there and
    // missing from the mask would make two different shaders share a DKSH.
    const rex::deko3d::GlslDeko g = rex::deko3d::SpirvAGlsl(shaders[i].spirv, 0);
    uint32_t leidos = 0;
    bool raro = false;
    for (size_t at = g.glsl.find("g_SpecConstants"); at != std::string::npos;
         at = g.glsl.find("g_SpecConstants", at + 1)) {
      const size_t ini = g.glsl.rfind('\n', at) + 1;
      const std::string linea = g.glsl.substr(ini, g.glsl.find('\n', at) - ini);
      unsigned valor = 0;
      if (linea.find("const uint g_SpecConstants = ") == 0) continue;
      if (std::sscanf(linea.c_str() + (at - ini), "g_SpecConstants & %uu)", &valor) == 1) {
        leidos |= valor;
      } else if (std::sscanf(linea.c_str() + (at - ini), "g_SpecConstants == %uu)", &valor) == 1 &&
                 (valor & 0x7F000000u)) {
        continue;
      } else {
        raro = true;
      }
    }
    if (mascaras[i] != ~0u && (raro || (leidos & ~mascaras[i]))) {
      ++mascaras_erroneas;
      std::printf("MASCARA ERRONEA %s: SPIR-V %08X, GLSL lee %08X%s\n", nombre(i).c_str(), mascaras[i], leidos,
                  raro ? " y la usa de otra forma" : "");
    }
  }
  // The distinct variants. The cache key: the value masked to what the shader reads, and the kept interpolators of
  // a vertex shader.
  struct Tarea {
    Variante v;
    rex::deko3d::CacheDksh::Clave clave;
  };
  std::vector<Tarea> tareas;
  {
    std::set<rex::deko3d::CacheDksh::Clave> hechas;
    for (const Variante& v : trabajo) {
      const auto& s = shaders[v.shader];
      const rex::deko3d::CacheDksh::Clave clave{rex::deko3d::HuellaSpirv(s.spirv),
                                               v.especializacion & mascaras[v.shader], s.vertices ? v.salidas : 0u};
      if (hechas.insert(clave).second) tareas.push_back({v, clave});
    }
  }
  filas.resize(tareas.size());
  auto archivo_glsl = [&](const Tarea& t) -> fs::path {
    if (carpeta_glsl.empty()) return {};
    char sufijo[32];
    std::snprintf(sufijo, sizeof(sufijo), "_%08X_%08X", t.clave.especializacion, t.clave.variante);
    return carpeta_glsl / (nombre(t.v.shader) + sufijo + (shaders[t.v.shader].vertices ? ".vert" : ".frag"));
  };
  // A finished variant: its row, its uam lines counted by kind, its DKSH into the cache.
  auto anotar = [&](size_t i, Resultado resultado, Compilado c, double ms) {
    const Tarea& t = tareas[i];
    Fila& f = filas[i];
    f.shader = nombre(t.v.shader);
    f.especializacion = t.clave.especializacion;
    f.variante = t.clave.variante;
    f.resultado = resultado == Resultado::kOk && !c.ok ? Resultado::kFallo : resultado;
    f.ms = ms;
    f.bytes = c.dksh.size();
    f.texturas = c.texturas;
    f.ubos = c.ubos;
    f.motivo = std::move(c.motivo);
    avisos_conversion += c.avisos_conversion;
    // uam's lines by kind: without the "0:91(35): " position, up to the first name or number in quotes
    size_t at = 0;
    while (at < c.log.size()) {
      size_t fin = c.log.find('\n', at);
      if (fin == std::string::npos) fin = c.log.size();
      std::string linea = c.log.substr(at, fin - at);
      if (const size_t pos = linea.find("): "); pos != std::string::npos && pos < 16) linea.erase(0, pos + 3);
      const size_t corte = linea.find_first_of("'`\"");
      if (!UnaLinea(linea).empty()) ++avisos_uam[UnaLinea(linea.substr(0, std::min<size_t>(corte, 80)))];
      at = fin + 1;
    }
    if (f.resultado == Resultado::kOk) cache.Anadir(t.clave, std::move(c.dksh));
  };

#ifdef _WIN32
  // No fork on Windows: one variant after another in this process, without a time limit.
  for (size_t i = 0; i < tareas.size(); ++i) {
    const Tarea& t = tareas[i];
    const auto& s = shaders[t.v.shader];
    const auto t0 = std::chrono::steady_clock::now();
    Compilado c = Compilar(s.spirv, t.clave.especializacion, s.vertices ? t.v.salidas : ~0u, archivo_glsl(t));
    anotar(i, Resultado::kOk, std::move(c),
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
#else
  /*
   * One child process per variant, up to `hilos` at once, each killed after `limite_s`. uam is not reentrant and
   * its register allocator can run for a very long time on one shader (Dead Space 2's ps_ae3ade667ec0386b sat
   * in nv50_ir::GCRA for over nine minutes), so a separate process is both the parallelism and the time limit.
   * Mesa's front end is built once here, before the first fork, so the children inherit it.
   */
  uam::compile(uam::Stage::Fragment, "#version 460\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(0.0); }\n");
  struct Hijo {
    size_t tarea = 0;
    int fd = -1;
    std::chrono::steady_clock::time_point inicio;
    std::string datos;
    bool matado = false;
  };
  std::map<pid_t, Hijo> hijos;
  size_t siguiente = 0, terminadas = 0;
  auto ultimo_aviso = std::chrono::steady_clock::now();
  std::printf("%zu variantes distintas en %u procesos, limite %.0f s por variante\n", tareas.size(), hilos, limite_s);
  std::fflush(stdout);
  while (siguiente < tareas.size() || !hijos.empty()) {
    while (siguiente < tareas.size() && hijos.size() < hilos) {
      int tubo[2];
      if (pipe(tubo) != 0) throw std::runtime_error("pipe fallo");
      std::fflush(stdout);
      const pid_t pid = fork();
      if (pid < 0) throw std::runtime_error("fork fallo");
      const Tarea& t = tareas[siguiente];
      if (pid == 0) {
        close(tubo[0]);
        const auto& s = shaders[t.v.shader];
        const std::string d = Serializar(
            Compilar(s.spirv, t.clave.especializacion, s.vertices ? t.v.salidas : ~0u, archivo_glsl(t)));
        for (size_t at = 0; at < d.size();) {
          const ssize_t n = write(tubo[1], d.data() + at, d.size() - at);
          if (n <= 0) _exit(3);
          at += size_t(n);
        }
        _exit(0);
      }
      close(tubo[1]);
      hijos[pid] = {siguiente++, tubo[0], std::chrono::steady_clock::now(), {}, false};
    }
    // Read whatever the children wrote (a DKSH can be larger than the pipe buffer), 100 ms at most.
    std::vector<pollfd> fds;
    for (const auto& [pid, h] : hijos)
      if (h.fd >= 0) fds.push_back({h.fd, POLLIN, 0});
    if (!fds.empty()) poll(fds.data(), fds.size(), 100);
    const auto ahora = std::chrono::steady_clock::now();
    for (auto it = hijos.begin(); it != hijos.end();) {
      Hijo& h = it->second;
      if (h.fd >= 0) {
        char buf[65536];
        for (;;) {
          pollfd p{h.fd, POLLIN, 0};
          if (poll(&p, 1, 0) <= 0) break;
          const ssize_t n = read(h.fd, buf, sizeof(buf));
          if (n <= 0) {
            close(h.fd);
            h.fd = -1;
            break;
          }
          h.datos.append(buf, size_t(n));
        }
      }
      const double ms = std::chrono::duration<double, std::milli>(ahora - h.inicio).count();
      if (!h.matado && ms > limite_s * 1000) {
        kill(it->first, SIGKILL);
        h.matado = true;
      }
      int estado = 0;
      if (h.fd >= 0 || waitpid(it->first, &estado, h.matado ? 0 : WNOHANG) != it->first) {
        ++it;
        continue;
      }
      Compilado c;
      Resultado r = Resultado::kOk;
      char motivo[128];
      if (h.matado) {
        r = Resultado::kTiempo;
        std::snprintf(motivo, sizeof(motivo), "TIEMPO AGOTADO: matado tras %.0f s", ms / 1000);
        c.motivo = motivo;
      } else if (WIFSIGNALED(estado)) {
        r = Resultado::kSenal;
        std::snprintf(motivo, sizeof(motivo), "SENAL: el proceso murio con la senal %d", WTERMSIG(estado));
        c.motivo = motivo;
      } else if (!WIFEXITED(estado) || WEXITSTATUS(estado) != 0 || !Deserializar(h.datos, c)) {
        r = Resultado::kSenal;
        std::snprintf(motivo, sizeof(motivo), "SENAL: el proceso salio con %d sin una respuesta completa",
                      WIFEXITED(estado) ? WEXITSTATUS(estado) : -1);
        c = {};
        c.motivo = motivo;
      }
      if (r != Resultado::kOk) {
        std::printf("%s %s %08X %08X\n", c.motivo.c_str(), nombre(tareas[h.tarea].v.shader).c_str(),
                    tareas[h.tarea].clave.especializacion, tareas[h.tarea].clave.variante);
      }
      anotar(h.tarea, r, std::move(c), ms);
      ++terminadas;
      it = hijos.erase(it);
    }
    if (ahora - ultimo_aviso > std::chrono::seconds(30)) {
      ultimo_aviso = ahora;
      std::printf("  %zu de %zu variantes\n", terminadas, tareas.size());
      std::fflush(stdout);
    }
  }
#endif
  uam::shutdown();

  const std::vector<uint8_t> archivo = cache.Serializar();
  Escribir(salida, archivo.data(), archivo.size());

  size_t fallos = 0, por_resultado[4] = {};
  std::vector<double> tiempos;
  for (const Fila& f : filas) {
    ++por_resultado[size_t(f.resultado)];
    fallos += f.resultado != Resultado::kOk;
    // Out of time and killed ones say nothing about how long a compile takes.
    if (f.resultado == Resultado::kOk || f.resultado == Resultado::kFallo) tiempos.push_back(f.ms);
    if (f.resultado == Resultado::kFallo)
      std::printf("FALLO %s %08X %08X: %s\n", f.shader.c_str(), f.especializacion, f.variante, f.motivo.c_str());
  }
  std::sort(tiempos.begin(), tiempos.end());
  auto pct = [&](double p) { return tiempos.empty() ? 0.0 : tiempos[std::min(tiempos.size() - 1, size_t(p * tiempos.size()))]; };
  std::printf("\n%zu variantes de %zu shaders: %zu distintas con la mascara de cada shader, %zu compiladas, %zu fallidas"
              " (%zu rechazadas, %zu fuera de tiempo, %zu procesos muertos)\n",
              trabajo.size(), shaders.size(), filas.size(), por_resultado[size_t(Resultado::kOk)], fallos,
              por_resultado[size_t(Resultado::kFallo)], por_resultado[size_t(Resultado::kTiempo)],
              por_resultado[size_t(Resultado::kSenal)]);
  for (const Fila& f : filas)
    if (f.resultado == Resultado::kTiempo || f.resultado == Resultado::kSenal)
      std::printf("  %-6s %s %08X %08X: %s\n", NombreResultado(f.resultado), f.shader.c_str(), f.especializacion,
                  f.variante, f.motivo.c_str());
  size_t sin_mascara = 0, vs_sin_bits = 0, vs = 0;
  for (size_t i = 0; i < shaders.size(); ++i) {
    sin_mascara += mascaras[i] == ~0u;
    vs += shaders[i].vertices;
    vs_sin_bits += shaders[i].vertices && mascaras[i] == 0;
  }
  std::printf("mascaras: %zu de %zu vertex shaders no leen ningun bit; %zu shaders sin mascara (usos desconocidos)\n",
              vs_sin_bits, vs, sin_mascara);
  std::printf("tiempo por variante (SPIR-V -> GLSL -> DKSH): p50 %.1f ms, p90 %.1f, p99 %.1f, max %.1f\n", pct(0.5),
              pct(0.9), pct(0.99), tiempos.empty() ? 0.0 : tiempos.back());
  std::printf("avisos de la conversion (componente dinamico de un vector de UBO): %zu\n", avisos_conversion);
  std::printf("lineas de uam por tipo:%s\n", avisos_uam.empty() ? " ninguna" : "");
  for (const auto& [tipo, n] : avisos_uam) std::printf("  %6zu  %s\n", n, tipo.c_str());
  std::printf("cache: %zu DKSH, %zu bytes en %s\n  id: %s\n", cache.size(), archivo.size(), salida.string().c_str(),
              rex::deko3d::IdCompilador().c_str());

  if (!informe.empty()) {
    std::ofstream t(informe);
    t << "shader\tespecializacion\tvariante\tok\tresultado\tms\tbytes_dksh\ttexturas\tubos\tmotivo\n";
    for (const Fila& f : filas) {
      char linea[160];
      std::snprintf(linea, sizeof(linea), "\t%08X\t%08X\t%d\t%s\t%.2f\t%zu\t%08X\t%04X\t", f.especializacion,
                    f.variante, int(f.resultado == Resultado::kOk), NombreResultado(f.resultado), f.ms, f.bytes,
                    f.texturas, f.ubos);
      t << f.shader << linea << f.motivo << '\n';
    }
  }
  if (mascaras_erroneas) std::printf("%zu mascaras erroneas\n", mascaras_erroneas);
  return fallos || mascaras_erroneas ? 2 : 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "error: %s\n", e.what());
  return 1;
}
