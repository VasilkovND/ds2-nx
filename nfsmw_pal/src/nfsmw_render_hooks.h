// NFS Most Wanted's plug-in for the universal deko renderer: its image settings and the classification of its own
// passes (the race composite, the sky and the glow bright pass). The renderer calls these; a game with no effects
// uses rex::x360::HooksPorDefecto() instead.
#pragma once

#include <rex/x360/render_hooks.h>

namespace nfsmw {
rex::x360::RenderHooks& HooksNfsmw();
}
