// nfsmw - deko3d renderer: the renderer backend of the graphics system (DestinosNativos) on deko3d. Switch only.
//
// nfsmw_nativo_sistema.cpp reads the game's command ring and hands every draw, copy (resolve and clear) and Swap to
// a DestinosNativos (nfsmw_nativo_destinos.h); the Vulkan one is nfsmw_nativo_destinos.cpp. This is the deko3d
// one, on the ring thread with its own Nucleo.
//
// The implementation is RenderDk (nfsmw_dk_render.h): render targets, copies and the output pass in
// nfsmw_dk_destinos.cpp, draws in nfsmw_dk_dibujos.cpp, textures in nfsmw_dk_texturas.cpp.

#pragma once

#include <memory>

#include <rex/x360/destinos.h>
#include <rex/x360/render_hooks.h>

// Forward-declare in the SDK namespace (rex::deko3d) and import, so this is the real SDK type, not a phantom
// local to nfsmw::deko that would shadow it.
namespace rex::deko3d {
class ProveedorDeko3d;
}
// `deko::X` in this renderer means the SDK deko3d layer.
namespace deko = ::rex::deko3d;

namespace rex::x360 {

std::unique_ptr<DestinosNativos> CrearDestinosDeko3d(deko::ProveedorDeko3d* proveedor,
                                                     rex::memory::Memory* memoria,
                                                     rex::x360::RenderHooks& hooks = rex::x360::HooksPorDefecto());

}  // namespace rex::x360
