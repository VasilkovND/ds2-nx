// nfsmw - environment variables for Mesa/NVK before creating Vulkan (nfsmw_entorno_mesa.cpp)

#pragma once

namespace nfsmw::entorno {

// Sets the variables from nfsmw_mesa_entorno and, on the Switch, logs whether Mesa's on-disk shader cache
// exists and how big it is. Must be called with the toml already read and before SetupPresentation
// (OnPostInitLogging).
void AplicarEntornoMesa();

}  // namespace nfsmw::entorno
