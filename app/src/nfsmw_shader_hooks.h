#pragma once
#include "nfsmw_shader_library.h"

namespace nfsmw::native {
// Initialize once before starting the guest threads. It is not reloaded.
void IniciarBibliotecaShaders();
// Only recognizes objects from the two verified creation paths. nullptr
// means unknown: it does not allow skipping the original draw.
const Shader* ShaderDeObjeto(uint32_t objeto);
const Shader* ShaderOriginal(std::span<const uint8_t> contenedor);
}  // namespace nfsmw::native
