// deadspace2 - Dead Space 2 (Xbox 360, TU2) on the Nintendo Switch.
// The recompiled code is Dead-Space-2-Recomp's src_tu2/ (see ../cmake/ds2_switch.cmake).

#include "ds2_rex_diagnostic_init.h"

#include "ds2_app.h"

// The project name is the one src_tu2 was generated against (its init/register files use it).
REX_DEFINE_APP(ds2_rex_diagnostic, Ds2App::Create)
