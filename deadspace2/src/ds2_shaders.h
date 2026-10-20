// deadspace2 - the shader library on the console: which translated shader each IM_LOAD is.
//
// The library (ds2_shaders.dslib next to the NRO) is the one shaders/ builds from shaders_dump/: each original
// container with its SPIR-V. Each entry becomes the rex::x360::EntradaShader the deko3d renderer draws with.
//
// Identification. A pixel shader arrives in the ring as it is in its container. A vertex shader does not: the
// XDK's Direct3D patches its vertex fetches for each vertex declaration, and sometimes copies it into the ring
// itself (IM_LOAD_IMMEDIATE). So each load is looked up, in order:
//   1. by address: the hooks on D3DDevice_Create{Vertex,Pixel}Shader (ds2_shader_dump.cpp) know the container
//      and, scanning the new object for a pointer to a copy of its microcode, where D3D keeps it;
//   2. by the fingerprint of the patched microcode, learned from loads that step 1 identified (the patch is the
//      same each time for the same shader and declaration);
//   3. by the microcode with the fetch instructions masked (enough for pixel shaders);
//   4. (vertex shaders) by similarity: the fetch instructions ignored whole (D3D may reorder them) and few other
//      words different (the exports it disables), the closest candidate if it is clearly the closest.
// Every step is counted: the log says how many loads each one identified and how many were left unknown.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <rex/x360/shader_entry.h>

namespace rex::memory {
class Memory;
}

namespace ds2::shaders {

struct Estadisticas {
  uint64_t cargas = 0;          // IM_LOAD / IM_LOAD_IMMEDIATE of a shader
  uint64_t por_direccion = 0;   // step 1
  uint64_t por_aprendizaje = 0; // step 2
  uint64_t por_microcodigo = 0; // step 3
  uint64_t por_parecido = 0;    // step 4: distinct patched VS matched by similarity
  uint64_t desconocidas = 0;
  uint64_t objetos = 0;            // shaders created by the game (create hooks)
  uint64_t objetos_sin_entrada = 0;  // their container is not in the library
  uint64_t objetos_sin_direccion = 0;  // no pointer to the microcode found in the object
};

class Biblioteca {
 public:
  Biblioteca();
  ~Biblioteca();

  bool Cargar(const std::filesystem::path& archivo);
  bool cargada() const;

  // The entry of an original container (the create hooks, before the call).
  const rex::x360::EntradaShader* PorContenedor(std::span<const uint8_t> contenedor) const;

  // After D3DDevice_Create*Shader returned `objeto` (guest address) for `entrada`: finds where the object keeps
  // its microcode and remembers that address. Any thread.
  void RegistrarObjeto(rex::memory::Memory* memoria, uint32_t objeto, const rex::x360::EntradaShader* entrada);

  // The ring's view of an IM_LOAD (fisica = its physical address) or an IM_LOAD_IMMEDIATE (fisica = 0), with the
  // microcode in host byte order. nullptr if no step identifies it. Ring thread.
  const rex::x360::EntradaShader* Identificar(bool vertices, uint32_t fisica, std::span<const uint32_t> microcodigo);

  Estadisticas estadisticas() const;

 private:
  struct Datos;
  std::unique_ptr<Datos> datos_;
};

// The one the game uses: loaded by the graphics system, fed by the create hooks.
Biblioteca& Global();

}  // namespace ds2::shaders
