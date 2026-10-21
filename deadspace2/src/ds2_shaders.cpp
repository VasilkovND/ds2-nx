// deadspace2 - the shader library on the console (see ds2_shaders.h).
//
// Dead Space 2's containers are the 2008 format (XenosRecomp shader.h, signature 0x102A1100/1101): the shader
// header is at +24, the microcode is `size` bytes at `physicalOffset`
// of the physical part, the vertex elements follow the header at +0x24, the pixel shader outputs are at +0x1C.
// The constant table is the same. All fields are big-endian.

#include "ds2_shaders.h"

#include "ds_shader_library.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

// xxHash reads with memcpy under GCC's strict aliasing (see shaders/ds_shader_library.cpp).
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace ds2::shaders {
namespace {

using rex::x360::ElementoVertice;
using rex::x360::EntradaShader;
using rex::x360::SamplerShader;

// Bits of a vertex fetch instruction that D3D does not patch.
constexpr uint32_t kFetchConserva[3] = {0x0007FFFF, 0x80000FFF, 0x80000000};
// Words of a new shader object searched for the pointer to its microcode.
constexpr uint32_t kPalabrasObjeto = 48;
constexpr uint32_t kMaxAvisos = 24;

struct Contenedor {
  const std::vector<uint8_t>& o;
  bool Hay(size_t p, size_t n) const { return p <= o.size() && n <= o.size() - p; }
  uint32_t U32(size_t p) const {
    return uint32_t(o[p]) << 24 | uint32_t(o[p + 1]) << 16 | uint32_t(o[p + 2]) << 8 | o[p + 3];
  }
  uint16_t U16(size_t p) const { return uint16_t(uint32_t(o[p]) << 8 | o[p + 1]); }
};

// OpKill, OpTerminateInvocation, OpDemoteToHelperInvocation.
uint32_t ContarKills(std::span<const uint32_t> spirv) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u) return 2;
  uint32_t kills = 0;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t palabras = spirv[i] >> 16, codigo = spirv[i] & 0xFFFF;
    if (palabras == 0 || i + palabras > spirv.size()) return 2;
    if (codigo == 252 || codigo == 4416 || codigo == 5380) ++kills;
    i += palabras;
  }
  return kills;
}

// The 2008 container into an entry. Returns the reason if it cannot be read.
const char* Leer(const ds::Shader& shader, EntradaShader& e) {
  const Contenedor c{shader.original};
  if (!c.Hay(0, 36)) return "contenedor demasiado corto";
  const uint32_t virtuales = c.U32(4);
  const uint32_t tabla = c.U32(16);
  const uint32_t cabecera = c.U32(24);
  if (size_t(cabecera) + (shader.vertices ? 0x24 : 0x20) > virtuales) return "cabecera fuera de la parte virtual";
  const uint32_t desplazamiento = c.U32(cabecera);
  const uint32_t bytes = c.U32(cabecera + 4);
  if (!bytes || (bytes % 4) || !c.Hay(size_t(virtuales) + desplazamiento, bytes)) {
    return "microcodigo fuera del contenedor";
  }
  e.vertices = shader.vertices;
  e.microcodigo.resize(bytes / 4);
  for (size_t i = 0; i < e.microcodigo.size(); ++i) {
    e.microcodigo[i] = c.U32(size_t(virtuales) + desplazamiento + i * 4);
  }

  if (e.vertices) {
    const uint32_t previos = c.U32(cabecera + 0x18);
    const uint32_t cantidad = c.U32(cabecera + 0x1C);
    const size_t comienzo = size_t(cabecera) + 0x24 + size_t(previos) * 4;
    if (cantidad > 64 || previos > 1024 || comienzo + size_t(cantidad) * 4 > virtuales) {
      return "elementos de vertices fuera de la parte virtual";
    }
    for (uint32_t i = 0; i < cantidad; ++i) {
      const uint32_t valor = c.U32(comienzo + size_t(i) * 4);
      ElementoVertice elemento;
      elemento.instruccion = uint16_t(valor & 0xFFF);
      elemento.uso = uint8_t((valor >> 12) & 0xF);
      elemento.indice_uso = uint8_t((valor >> 16) & 0xF);
      if ((size_t(elemento.instruccion) + 1) * 3 > e.microcodigo.size()) {
        return "elemento de vertices apunta fuera del microcodigo";
      }
      // A fetch whose index is not r0.x (the vertex's own index) is read by the shader itself (ElementoVertice::manual);
      // the translator (shaders/XenosRecomp) uses the same rule on the same word. The source register and its
      // component are not patched by D3D, but the mask below drops the component: read before it.
      const uint32_t w0 = e.microcodigo[size_t(elemento.instruccion) * 3];
      const uint32_t w1 = e.microcodigo[size_t(elemento.instruccion) * 3 + 1];
      elemento.manual = ((w0 >> 5) & 0x3F) != 0 || ((w0 >> 30) & 0x3) != 0;
      elemento.redondeado = (w1 >> 15) & 0x1;
      e.elementos.push_back(elemento);
      for (size_t j = 0; j < 3; ++j) e.microcodigo[size_t(elemento.instruccion) * 3 + j] &= kFetchConserva[j];
    }
  } else {
    e.salidas = c.U32(cabecera + 0x1C);
  }

  // Constant table: the float registers read and the samplers.
  if (!tabla || !c.Hay(size_t(tabla) + 4, 28)) return "sin tabla de constantes";
  const size_t base = size_t(tabla) + 4;
  const uint32_t constantes = c.U32(base + 12);
  const uint32_t info = c.U32(base + 16);
  if (constantes > 1024 || !c.Hay(base + info, size_t(constantes) * 20)) return "tabla de constantes fuera";
  uint32_t registros_float = 0;
  for (uint32_t i = 0; i < constantes; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) == 2) {  // RegisterSet::Float4
      const uint32_t indice = c.U16(p + 6), cuantos = c.U16(p + 8);
      registros_float = std::max(registros_float, cuantos > 1 ? (e.vertices ? 256u : 224u) : indice + 1);
    }
  }
  e.constantes_bytes = std::min<uint32_t>(std::max<uint32_t>(registros_float, 1), 256) * 16;
  for (uint32_t i = 0; i < constantes; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) != 3) continue;  // RegisterSet::Sampler
    SamplerShader sampler;
    sampler.registro = c.U16(p + 6);
    const uint32_t tipo = c.U32(p + 12);
    sampler.tipo = c.Hay(base + tipo, 4) ? c.U16(base + tipo + 2) : 0;
    e.samplers.push_back(sampler);
  }
  e.huella = XXH3_64bits(e.microcodigo.data(), e.microcodigo.size() * sizeof(uint32_t));
  return nullptr;
}

// The dimension each texture binding really has in the SPIR-V: the translator makes a sampler a cube when every fetch
// of it is a cube, whatever the constant table says (Dead Space 2's g_envSampler is declared sampler2D), and the
// renderer checks the fetch constant against the sampler's type. deko3d bindings: set 0, binding = register.
void TiposDesdeSpirv(std::span<const uint32_t> spirv, std::vector<SamplerShader>& samplers) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u || samplers.empty()) return;
  std::unordered_map<uint32_t, uint32_t> dimension;  // OpTypeImage id -> Dim
  std::unordered_map<uint32_t, uint32_t> apunta;     // OpTypePointer id -> pointee
  std::unordered_map<uint32_t, uint32_t> tipo_var;   // OpVariable id -> its pointer type
  std::unordered_map<uint32_t, uint32_t> binding, conjunto;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t palabras = spirv[i] >> 16, op = spirv[i] & 0xFFFF;
    if (!palabras || i + palabras > spirv.size()) return;
    const uint32_t* w = &spirv[i];
    if (op == 25 && palabras >= 4) dimension[w[1]] = w[3];           // OpTypeImage
    else if (op == 32 && palabras >= 4) apunta[w[1]] = w[3];         // OpTypePointer
    else if (op == 59 && palabras >= 4) tipo_var[w[2]] = w[1];       // OpVariable
    else if (op == 71 && palabras >= 4 && w[2] == 33) binding[w[1]] = w[3];   // OpDecorate Binding
    else if (op == 71 && palabras >= 4 && w[2] == 34) conjunto[w[1]] = w[3];  // OpDecorate DescriptorSet
    i += palabras;
  }
  for (const auto& [var, b] : binding) {
    const auto c = conjunto.find(var);
    if (c == conjunto.end() || c->second != 0) continue;
    const auto t = tipo_var.find(var);
    if (t == tipo_var.end()) continue;
    const auto p = apunta.find(t->second);
    if (p == apunta.end()) continue;
    const auto d = dimension.find(p->second);
    if (d == dimension.end()) continue;
    for (SamplerShader& sampler : samplers) {
      if (sampler.registro != b) continue;
      if (d->second == 3) sampler.tipo = 14;       // Dim Cube: SamplerCube
      else if (d->second == 2) sampler.tipo = 13;  // Dim 3D: Sampler3D
    }
  }
}

uint64_t Huella(bool vertices, std::span<const uint32_t> microcodigo) {
  return XXH3_64bits(microcodigo.data(), microcodigo.size_bytes()) ^ (uint64_t(microcodigo.size()) << 1) ^
         uint64_t(vertices);
}

}  // namespace

struct Biblioteca::Datos {
  ds::BibliotecaShaders biblioteca;
  std::vector<EntradaShader> entradas;
  std::unordered_map<const void*, uint32_t> por_shader;
  std::map<std::pair<bool, uint32_t>, std::vector<uint32_t>> candidatos;  // (vertices, words) -> entries
  bool cargada = false;

  std::mutex mutex;  // the maps below: the create hooks run on game threads, IM_LOAD on the ring thread
  std::unordered_map<uint32_t, const EntradaShader*> por_direccion;  // physical address of the microcode
  // Step 1's answers already checked against the code at that address: the next load from it (about 50,000 a second,
  // most of them repeats) is recognised by its length and three of its words, without hashing the microcode.
  struct Validada {
    const EntradaShader* entrada;
    uint32_t palabras, primera, media, ultima;
  };
  std::unordered_map<uint32_t, Validada> validadas;
  std::unordered_map<uint64_t, const EntradaShader*> aprendidas;     // Huella(patched microcode)
  std::unordered_map<uint64_t, const EntradaShader*> por_microcodigo;  // step 3 results, nullptr included
  std::vector<uint32_t> temporal;
  Estadisticas estadisticas;
  uint32_t avisos_objetos = 0, avisos_cargas = 0, avisos_direccion = 0, avisos_parecido = 0;
};

Biblioteca::Biblioteca() : datos_(std::make_unique<Datos>()) {}
Biblioteca::~Biblioteca() = default;

bool Biblioteca::cargada() const { return datos_->cargada; }

bool Biblioteca::Cargar(const std::filesystem::path& archivo) {
  Datos& d = *datos_;
  try {
    d.biblioteca.Cargar(archivo);
  } catch (const std::exception& e) {
    REXLOG_WARN("[shaders] library not available ({}): {}", archivo.string(), e.what());
    return false;
  }
  const auto& shaders = d.biblioteca.shaders();
  d.entradas.clear();
  d.entradas.reserve(shaders.size());
  uint32_t ignorados = 0;
  for (uint32_t i = 0; i < shaders.size(); ++i) {
    EntradaShader e;
    e.juego = &shaders[i];
    e.spirv = shaders[i].spirv;
    e.huella_contenedor = shaders[i].huella;
    e.numero = i;
    if (const char* motivo = Leer(shaders[i], e)) {
      if (++ignorados <= 8) REXLOG_WARN("[shaders] container {} skipped: {}", i, motivo);
      continue;
    }
    TiposDesdeSpirv(e.spirv, e.samplers);
    if (!e.vertices) {
      e.kills = ContarKills(e.spirv);
      e.descarta = e.kills != 1;  // 1 = only the alpha test's
    }
    d.entradas.push_back(std::move(e));
  }
  for (uint32_t i = 0; i < d.entradas.size(); ++i) {
    const EntradaShader& e = d.entradas[i];
    d.candidatos[{e.vertices, uint32_t(e.microcodigo.size())}].push_back(i);
    d.por_shader[e.juego] = i;
  }
  d.cargada = !d.entradas.empty();
  const size_t vs = std::count_if(d.entradas.begin(), d.entradas.end(), [](const auto& e) { return e.vertices; });
  REXLOG_INFO("[shaders] library {}: {} shaders ({} VS, {} PS), {} skipped", archivo.string(), d.entradas.size(),
              vs, d.entradas.size() - vs, ignorados);
  return d.cargada;
}

const EntradaShader* Biblioteca::PorContenedor(std::span<const uint8_t> contenedor) const {
  const Datos& d = *datos_;
  if (!d.cargada) return nullptr;
  const ds::Shader* shader = d.biblioteca.Buscar(contenedor);
  if (!shader) return nullptr;
  const auto it = d.por_shader.find(shader);
  return it != d.por_shader.end() ? &d.entradas[it->second] : nullptr;
}

void Biblioteca::RegistrarObjeto(rex::memory::Memory* memoria, uint32_t objeto, const EntradaShader* entrada) {
  Datos& d = *datos_;
  std::lock_guard<std::mutex> lock(d.mutex);
  ++d.estadisticas.objetos;
  if (!entrada) {
    ++d.estadisticas.objetos_sin_entrada;
    return;
  }
  if (!memoria || !objeto) {
    ++d.estadisticas.objetos_sin_direccion;
    return;
  }
  // The microcode as it is in the container (big-endian, unpatched): D3D has just copied it, or points at it.
  const ds::Shader& shader = *static_cast<const ds::Shader*>(entrada->juego);
  const Contenedor c{shader.original};
  const uint32_t virtuales = c.U32(4), cabecera = c.U32(24);
  const uint32_t desplazamiento = c.U32(cabecera);  // of the microcode in the physical part
  const uint32_t bytes = uint32_t(entrada->microcodigo.size() * 4);
  const uint32_t comparar = std::min<uint32_t>(bytes, 64);
  auto legible = [&](uint32_t direccion) {
    rex::memory::BaseHeap* heap = memoria->LookupHeap(direccion);
    uint32_t proteccion = 0;
    return heap && heap->QueryProtect(direccion, &proteccion) && (proteccion & rex::memory::kMemoryProtectRead);
  };
  const uint8_t* palabras = memoria->TranslateVirtual<const uint8_t*>(objeto);
  // The object points either at the microcode or at the start of its copy of the physical part (Dead Space 2: word 6,
  // with the microcode `physicalOffset` bytes in).
  for (uint32_t k = 0; k < kPalabrasObjeto * 2; ++k) {
    const uint32_t i = k / 2;
    const uint32_t puntero = rex::memory::load_and_swap<uint32_t>(palabras + size_t(i) * 4);
    if (!puntero || (puntero & 0x3) || ((k & 1) && !desplazamiento)) continue;
    const uint32_t v = puntero + ((k & 1) ? desplazamiento : 0);
    if (!legible(v) || !legible(v + comparar - 1)) continue;
    // With a vertex shader, D3D may already have patched the fetches: compare only what it leaves alone.
    const uint8_t* copia = memoria->TranslateVirtual<const uint8_t*>(v);
    bool igual = true;
    for (uint32_t w = 0; w < comparar / 4 && igual; ++w) {
      uint32_t a = rex::memory::load_and_swap<uint32_t>(copia + w * 4);
      uint32_t b = c.U32(size_t(virtuales) + c.U32(cabecera) + w * 4);
      if (entrada->vertices) {
        for (const ElementoVertice& el : entrada->elementos) {
          if (w / 3 == el.instruccion) {
            a &= kFetchConserva[w % 3];
            b &= kFetchConserva[w % 3];
          }
        }
      }
      igual = a == b;
    }
    if (!igual) continue;
    d.por_direccion[memoria->GetPhysicalAddress(v) & 0x1FFFFFFF] = entrada;
    d.validadas.erase(memoria->GetPhysicalAddress(v) & 0x1FFFFFFF);  // a new shader there: check it again
    return;
  }
  ++d.estadisticas.objetos_sin_direccion;
  if (d.avisos_objetos++ < 4) {
    std::string dump;
    for (uint32_t i = 0; i < 16; ++i) {
      dump += fmt::format(" {:08X}", rex::memory::load_and_swap<uint32_t>(palabras + size_t(i) * 4));
    }
    REXLOG_WARN("[shaders] {} object {:08X} (n{}): no pointer to its microcode in the first {} words:{}",
                entrada->vertices ? "VS" : "PS", objeto, entrada->numero, kPalabrasObjeto, dump);
  }
}

const EntradaShader* Biblioteca::Identificar(bool vertices, uint32_t fisica,
                                             std::span<const uint32_t> microcodigo) {
  Datos& d = *datos_;
  if (!d.cargada) return nullptr;
  std::lock_guard<std::mutex> lock(d.mutex);
  ++d.estadisticas.cargas;
  const size_t n = microcodigo.size();
  if (fisica && n) {
    if (auto it = d.validadas.find(fisica & 0x1FFFFFFF); it != d.validadas.end()) {
      const Datos::Validada& v = it->second;
      if (v.palabras == n && v.primera == microcodigo[0] && v.media == microcodigo[n / 2] &&
          v.ultima == microcodigo[n - 1]) {
        ++d.estadisticas.por_direccion;
        return v.entrada;
      }
    }
  }
  const uint64_t huella = Huella(vertices, microcodigo);
  // 1. By address, if the code there is still that shader's: the memory of a destroyed shader can hold another one
  // of the same length, and a wrong match here is also learned (step 2) for the rest of the session. The words
  // outside its fetch instructions must almost all be the entry's (D3D only rewrites fetches and a few exports).
  if (fisica) {
    if (auto it = d.por_direccion.find(fisica & 0x1FFFFFFF); it != d.por_direccion.end() &&
        it->second->vertices == vertices && it->second->microcodigo.size() == microcodigo.size()) {
      const EntradaShader& e = *it->second;
      std::vector<bool> fetch(microcodigo.size(), false);
      for (const ElementoVertice& el : e.elementos) {
        for (size_t j = 0; j < 3; ++j) fetch[size_t(el.instruccion) * 3 + j] = true;
      }
      size_t distintas = 0;
      for (size_t w = 0; w < microcodigo.size(); ++w) distintas += !fetch[w] && microcodigo[w] != e.microcodigo[w];
      if (distintas <= std::max<size_t>(3, microcodigo.size() / 8)) {
        ++d.estadisticas.por_direccion;
        if (vertices) d.aprendidas[huella] = it->second;
        d.validadas[fisica & 0x1FFFFFFF] = {it->second, uint32_t(n), microcodigo[0], microcodigo[n / 2],
                                            microcodigo[n - 1]};
        return it->second;
      }
      ++d.estadisticas.direccion_rechazada;
      if (d.avisos_direccion++ < kMaxAvisos) {
        REXLOG_WARN("[shaders] {} at {:08X} is no longer n{}: {} of {} words differ", vertices ? "VS" : "PS",
                    fisica, e.numero, distintas, microcodigo.size());
      }
    }
  }
  // 2. Learned patched microcode.
  if (auto it = d.aprendidas.find(huella); it != d.aprendidas.end()) {
    ++d.estadisticas.por_aprendizaje;
    return it->second;
  }
  // 3. The microcode with each candidate's fetches masked (cached, misses too).
  const EntradaShader* elegido = nullptr;
  bool parecido = false;
  if (auto it = d.por_microcodigo.find(huella); it != d.por_microcodigo.end()) {
    elegido = it->second;
  } else {
    if (auto c = d.candidatos.find({vertices, uint32_t(microcodigo.size())}); c != d.candidatos.end()) {
      for (uint32_t indice : c->second) {
        const EntradaShader& e = d.entradas[indice];
        d.temporal.assign(microcodigo.begin(), microcodigo.end());
        for (const ElementoVertice& el : e.elementos) {
          for (size_t j = 0; j < 3; ++j) d.temporal[size_t(el.instruccion) * 3 + j] &= kFetchConserva[j];
        }
        if (d.temporal == e.microcodigo) {
          elegido = &e;
          break;
        }
      }
    }
    // 4. A vertex shader D3D patched beyond its fetch bits: it may reorder the fetch instructions and rewrite the
    // exports the pixel shader does not read. Each candidate's fetch instructions are ignored whole and the rest
    // compared word by word; the closest one is taken if it is clearly the closest and differs in few words.
    if (!elegido && vertices) {
      if (auto c = d.candidatos.find({vertices, uint32_t(microcodigo.size())}); c != d.candidatos.end()) {
        size_t mejor = SIZE_MAX, segundo = SIZE_MAX;
        const EntradaShader* candidato = nullptr;
        for (uint32_t indice : c->second) {
          const EntradaShader& e = d.entradas[indice];
          std::vector<bool> fetch(microcodigo.size(), false);
          for (const ElementoVertice& el : e.elementos) {
            for (size_t j = 0; j < 3; ++j) fetch[size_t(el.instruccion) * 3 + j] = true;
          }
          size_t distintas = 0;
          for (size_t w = 0; w < microcodigo.size() && distintas <= mejor; ++w) {
            if (fetch[w]) continue;
            uint32_t a = microcodigo[w], b = e.microcodigo[w];
            distintas += a != b;
          }
          if (distintas < mejor) {
            segundo = mejor;
            mejor = distintas;
            candidato = &e;
          } else if (distintas < segundo) {
            segundo = distintas;
          }
        }
        if (candidato && mejor <= std::max<size_t>(3, microcodigo.size() / 8) && mejor < segundo) {
          elegido = candidato;
          parecido = true;
          if (d.avisos_parecido++ < kMaxAvisos) {
            REXLOG_WARN("[shaders] VS at {:08X} taken as n{} by similarity: {} words differ (next best {})", fisica,
                        candidato->numero, mejor, segundo);
          }
          d.aprendidas[huella] = elegido;  // later loads of it: step 2
        }
      }
    }
    d.por_microcodigo.emplace(huella, elegido);
    if (!elegido && d.avisos_cargas++ < kMaxAvisos) {
      REXLOG_WARN("[shaders] unknown {} load: {} words at {:08X}", vertices ? "VS" : "PS", microcodigo.size(),
                  fisica);
    }
  }
  ++(parecido ? d.estadisticas.por_parecido
              : elegido ? d.estadisticas.por_microcodigo : d.estadisticas.desconocidas);
  return elegido;
}

Estadisticas Biblioteca::estadisticas() const {
  std::lock_guard<std::mutex> lock(datos_->mutex);
  return datos_->estadisticas;
}

Biblioteca& Global() {
  static Biblioteca biblioteca;
  return biblioteca;
}

}  // namespace ds2::shaders
