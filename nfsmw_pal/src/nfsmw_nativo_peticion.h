// NFS bridge to the universal Xbox 360 SDK renderer types (rex::x360). NFS's renderer namespace is nfsmw::nativo;
// these imports let its existing code use the shared types unqualified. A new game includes <rex/x360/...> directly.
#pragma once
#include <rex/x360/peticion.h>
namespace nfsmw::nativo { using namespace ::rex::x360; }
