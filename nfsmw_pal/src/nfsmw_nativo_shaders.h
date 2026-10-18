// nfsmw - native renderer, part C5a: identify in the NFSSPV library the
// shaders the game's D3D uploads to the ring (PM4_IM_LOAD and
// IM_LOAD_IMMEDIATE).
//
// The ring only carries the microcode. The library stores the original
// compiled container (header, constant table, definitions and microcode)
// with its SPIR-V translation made by XenosRecomp.
//
// Pixel shaders arrive as is and are identified by their microcode. Vertex
// shaders do not: D3D reorders the fetches, changes their swizzles and
// nulls the outputs the pixel shader does not read (measured), so their
// identity comes from the device objects (nfsmw_nativo_ganchos.h) and only
// the vertex input is read from the patched microcode.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace nfsmw::native {
struct Shader;
}

// ElementoVertice, SamplerShader and EntradaShader are the universal SDK descriptor (rex::x360). The game's library
// fills spirv/huella/huella_contenedor and stashes its own Shader pointer in juego.
#include <rex/x360/shader_entry.h>

namespace nfsmw::nativo {
using namespace ::rex::x360;

struct EstadisticasShaders {
  uint64_t cargas = 0;           // IM_LOAD recibidos
  uint64_t distintos = 0;        // microcodigos distintos (con sus parches)
  uint64_t identificados = 0;    // of the distinct ones
  uint64_t sin_identificar = 0;  // of the distinct ones
  uint64_t ambiguos = 0;         // distinct ones with more than one possible container
};

class ShadersNativos {
 public:
  ShadersNativos();
  ~ShadersNativos();

  // false, with a warning in the log, if the library is missing or damaged.
  bool Cargar(const std::filesystem::path& archivo);
  bool cargada() const;

  // Full original container (the one the D3D constructors receive).
  // nullptr if it is not in the library. Can be called from any thread
  // once the library is loaded.
  const EntradaShader* IdentificarContenedor(std::span<const uint8_t> contenedor) const;

  // The entry with that number (its position in the library), or nullptr. Like IdentificarContenedor, from any
  // thread once loaded: entries do not change after Cargar. Pipeline prewarming (nfsmw_nativo_dibujos.cpp) uses
  // it to recreate the pipelines of the previous session.
  const EntradaShader* PorNumero(uint32_t numero) const;

  // Microcode already in host byte order. nullptr if it is not in the library.
  // Only the ring thread uses it.
  const EntradaShader* Identificar(bool vertices, std::span<const uint32_t> microcodigo);

  EstadisticasShaders Estadisticas() const;

 private:
  struct Datos;
  std::unique_ptr<Datos> datos_;
};

// Short name of a DeclUsage for the reports.
const char* NombreUso(uint8_t uso);

// Whether a patched microcode from the ring can belong to that vertex shader: every
// declaration element has, at one of the element positions, a fetch that writes the
// same temporary register as the original. D3D reorders the fetches and changes
// formats and swizzles, but not the registers.
bool FetchCoherentes(const EntradaShader& vs, std::span<const uint32_t> parcheado);

}  // namespace nfsmw::nativo
