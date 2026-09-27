#include "../app/src/nfsmw_shader_vulkan.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#define XXH_INLINE_ALL
#include <xxhash.h>

using namespace nfsmw::native;
namespace {
void Comprobar(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
std::vector<uint8_t> Leer(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  Comprobar(bool(f) && f.tellg() >= 0, "No se pudo leer la entrada");
  std::vector<uint8_t> d(static_cast<size_t>(f.tellg())); f.seekg(0);
  Comprobar(bool(f.read(reinterpret_cast<char*>(d.data()), d.size())), "Lectura incompleta");
  return d;
}
uint32_t U32(const std::vector<uint8_t>& d, size_t p) {
  return uint32_t(d[p]) | uint32_t(d[p+1]) << 8 | uint32_t(d[p+2]) << 16 | uint32_t(d[p+3]) << 24;
}
void Poner(std::vector<uint8_t>& d, size_t p, uint32_t v) {
  for (size_t i = 0; i < 4; ++i) d.at(p+i) = uint8_t(v >> (8*i));
}
void Sellar(std::vector<uint8_t>& d) {
  const uint64_t h = XXH3_64bits(d.data() + 24, d.size() - 24);
  Poner(d, 16, uint32_t(h)); Poner(d, 20, uint32_t(h >> 32));
}
size_t creados = 0, destruidos = 0;
bool fallar = false;
VKAPI_ATTR VkResult VKAPI_CALL Crear(VkDevice, const VkShaderModuleCreateInfo* i,
                                     const VkAllocationCallbacks*, VkShaderModule* m) {
  Comprobar(i->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO &&
      i->codeSize >= 20 && i->codeSize % 4 == 0 &&
      uintptr_t(i->pCode) % alignof(uint32_t) == 0 && i->pCode[0] == 0x07230203,
      "Parametros incorrectos de vkCreateShaderModule");
  if (fallar) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  *m = (VkShaderModule)(uintptr_t(++creados));
  return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL Destruir(VkDevice, VkShaderModule m, const VkAllocationCallbacks*) {
  Comprobar(m != VK_NULL_HANDLE, "Se destruye un modulo nulo");
  ++destruidos;
}
}
int main(int argc, char** argv) try {
  Comprobar(argc == 3, "Uso: nfsmw_probar_biblioteca <paquete> <contenedores>");
  const auto original = Leer(argv[1]);
  BibliotecaShaders b; b.Cargar(original);
  const size_t cantidad = b.shaders().size();
  Comprobar(cantidad > 1, "Corpus insuficiente");
  size_t rechazados = 0, coincidencias = 0;
  auto rechazar = [&](std::vector<uint8_t> d, bool sellar = false) {
    if (sellar) Sellar(d);
    bool fallo = false;
    try { b.Cargar(d); } catch (const std::runtime_error&) { fallo = true; }
    Comprobar(fallo, "Se acepto una alteracion del paquete");
    Comprobar(b.shaders().size() == cantidad, "La carga fallida sustituyo la biblioteca");
    ++rechazados;
  };
  for (size_t n = 0; n < 64; ++n) rechazar({original.begin(), original.begin() + n});
  rechazar({original.begin(), original.end() - 1});
  auto d = original; d[0] ^= 1; rechazar(d);
  d = original; Poner(d, 8, 2); rechazar(d);
  d = original; Poner(d, 12, 4097); rechazar(d);
  d = original; Poner(d, 12, U32(d, 12) + 1); rechazar(d);
  d = original; d.back() ^= 1; rechazar(d);
  d = original; d.push_back(0); rechazar(d, true);
  d = original; Poner(d, 24, 0xffffffff); rechazar(d, true);
  d = original; Poner(d, 28, 0xffffffff); rechazar(d, true);
  d = original; d[32] ^= 1; rechazar(d, true);
  const size_t codigo = 40 + U32(original, 24);
  d = original; Poner(d, codigo, 0); rechazar(d, true);
  d = original; Poner(d, codigo + 20, 0); rechazar(d, true);
  size_t entrada = codigo + 20;
  while ((U32(original, entrada) & 65535) != 15) entrada += 4 * (U32(original, entrada) >> 16);
  d = original; Poner(d, entrada + 4, 5); rechazar(d, true);
  d = original; Poner(d, entrada + 12, 0); rechazar(d, true);
  for (const auto& e : std::filesystem::directory_iterator(argv[2])) {
    if (e.path().extension() != ".bin") continue;
    auto datos = Leer(e.path());
    const Shader* s = b.Buscar(datos);
    Comprobar(s && s->original == datos, "No se recupera un contenedor original");
    datos.back() ^= 1;
    Comprobar(!b.Buscar(datos), "Un contenedor alterado selecciona un shader");
    ++coincidencias;
  }
  auto copia = b.shaders();
  copia.push_back(copia.front()); std::reverse(copia.begin(), copia.end());
  Comprobar(EmpaquetarShaders(copia) == original, "El paquete no es determinista o no elimina duplicados");
  copia.back().spirv.back() ^= 1;
  bool fallo = false;
  try { (void)EmpaquetarShaders(copia); } catch (const std::runtime_error&) { fallo = true; }
  Comprobar(fallo, "Se aceptan traducciones distintas del mismo contenedor");
  {
    ModulosShaders m((VkDevice)(uintptr_t(1)), Crear, Destruir);
    VkShaderModule modulo;
    fallar = true;
    Comprobar(m.Obtener(b.shaders().front(), modulo) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
        modulo == VK_NULL_HANDLE, "Se oculta el error de Vulkan");
    fallar = false;
    for (const auto& s : b.shaders()) {
      VkShaderModule repetido;
      Comprobar(m.Obtener(s, modulo) == VK_SUCCESS && modulo != VK_NULL_HANDLE, "Fallo de creacion");
      Comprobar(m.Obtener(s, repetido) == VK_SUCCESS && repetido == modulo, "Fallo de cache");
    }
    Comprobar(creados == cantidad, "Se crean modulos repetidos");
  }
  Comprobar(destruidos == creados, "Fuga de modulos Vulkan");
  std::printf("%zu originales encontrados; %zu unicos; %zu paquetes incorrectos rechazados; cache, fallo y destruccion comprobados con Vulkan simulado\n",
      coincidencias, cantidad, rechazados);
  return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
