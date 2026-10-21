// deadspace2 - how the recompiled code's "interrupts off" regions behave (see tools/make_nx_pch.py, point 3).
//
// The game brackets its lwarx/stwcx. atomics with mtmsrd. The codegen made each region one process-wide mutex; the
// Switch PCH keeps that state per thread instead, which is enough for the atomics (host compare-and-swap) and lets the
// job scheduler's threads run side by side. ds2_msr_cerrojo_global brings the old mutex back, for comparing.

#include "ds2_rex_diagnostic_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ds2_cpu.h"

REXCVAR_DECLARE(bool, ignore_thread_priorities);  // the SDK's (src/system/xthread.cpp): on by default

REXCVAR_DEFINE_BOOL(ds2_msr_cerrojo_global, false, "DS2",
                    "Las regiones con interrupciones apagadas del codigo recompilado (mtmsrd) como un mutex global "
                    "del proceso, como las genera el codegen. false = estado por hilo (los atomicos ya son CAS)");

namespace ds2::cpu {

void Configurar() {
  ppc_msr_cerrojo_global_() = REXCVAR_GET(ds2_msr_cerrojo_global);
  REXLOG_INFO("[cpu] regiones mtmsrd: {}", ppc_msr_cerrojo_global_() ? "mutex global (codegen)" : "estado por hilo");
}

}  // namespace ds2::cpu

// EA::Thread::SetThreadPriority(int): the job scheduler's workers raise their own priority to 127 before each job and
// drop it to -128 after it. In the SDK kernel each call is a handle lookup, two object lookups and a release, every
// one under the object table's lock (the one every WaitForSingleObject and ReleaseSemaphore also takes), and with
// ignore_thread_priorities the host thread's priority does not change anyway. Then it only reports success.
REX_EXTERN(__imp__EA__Thread__SetThreadPriority);
REX_EXTERN(EA__Thread__SetThreadPriority) {
  if (REXCVAR_GET(ignore_thread_priorities)) {
    ctx.r3.u64 = 1;  // true
    return;
  }
  __imp__EA__Thread__SetThreadPriority(ctx, base);
}
