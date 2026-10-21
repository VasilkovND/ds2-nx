// deadspace2 - how the recompiled code's "interrupts off" regions behave (see tools/make_nx_pch.py, point 3).
//
// The game brackets its lwarx/stwcx. atomics with mtmsrd. The codegen made each region one process-wide mutex; the
// Switch PCH keeps that state per thread instead, which is enough for the atomics (host compare-and-swap) and lets the
// job scheduler's threads run side by side. ds2_msr_cerrojo_global brings the old mutex back, for comparing.

#include "ds2_rex_diagnostic_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ds2_cpu.h"

REXCVAR_DEFINE_BOOL(ds2_msr_cerrojo_global, false, "DS2",
                    "Las regiones con interrupciones apagadas del codigo recompilado (mtmsrd) como un mutex global "
                    "del proceso, como las genera el codegen. false = estado por hilo (los atomicos ya son CAS)");

namespace ds2::cpu {

void Configurar() {
  ppc_msr_cerrojo_global_() = REXCVAR_GET(ds2_msr_cerrojo_global);
  REXLOG_INFO("[cpu] regiones mtmsrd: {}", ppc_msr_cerrojo_global_() ? "mutex global (codegen)" : "estado por hilo");
}

}  // namespace ds2::cpu
