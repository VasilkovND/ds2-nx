// deadspace2 - how the recompiled code's "interrupts off" regions behave (see tools/make_nx_pch.py, point 3).
//
// The game brackets its lwarx/stwcx. atomics with mtmsrd. The codegen made each region one process-wide mutex; the
// Switch PCH keeps that state per thread instead, which is enough for the atomics (host compare-and-swap) and lets the
// job scheduler's threads run side by side. ds2_msr_cerrojo_global brings the old mutex back, for comparing.

#include "ds2_rex_diagnostic_pch.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

#include "ds2_cpu.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DECLARE(bool, ignore_thread_priorities);  // the SDK's (src/system/xthread.cpp): on by default
REXCVAR_DECLARE(bool, switch_atajos_depuracion);  // the SDK's ZL + ZR shortcuts (src/input/switch/switch_input_driver.cpp)

REXCVAR_DEFINE_BOOL(ds2_msr_cerrojo_global, false, "DS2",
                    "Las regiones con interrupciones apagadas del codigo recompilado (mtmsrd) como un mutex global "
                    "del proceso, como las genera el codegen. false = estado por hilo (los atomicos ya son CAS)");

namespace ds2::cpu {

void Configurar() {
#if !DS2_DEPURACION
  // A release build has no debug modes (CMakeLists.txt, DS2_DEPURACION): only the FPS counter, even when ds2.toml
  // turns the shortcuts on.
  if (REXCVAR_GET(switch_atajos_depuracion)) {
    REXCVAR_SET(switch_atajos_depuracion, false);
    REXLOG_INFO("[cpu] switch_atajos_depuracion ignorado: version de lanzamiento (solo el contador de FPS)");
  }
#endif
  ppc_msr_cerrojo_global_() = REXCVAR_GET(ds2_msr_cerrojo_global);
  REXLOG_INFO("[cpu] regiones mtmsrd: {}", ppc_msr_cerrojo_global_() ? "mutex global (codegen)" : "estado por hilo");
  REXLOG_INFO("[cpu] registros del invitado en variables locales (tools/patch_local_regs.py): {}", DS2_LOCAL_REGS);
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

// Direct3D's GPU wait step, D3D::CBlocker::Check (0x822187C8): BlockOnFence and the other Direct3D waits call it in a
// loop until the GPU (here the ring thread) has passed a fence; it answers 1 while still waiting. On the Xbox 360 its
// db16cyc delays cost one hardware thread of six; here they compile to nothing and the loop takes a whole core from the
// threads it waits for (the ring thread, the audio mixers, the job workers). While it answers "still waiting" the
// thread yields the first ds2_espera_gpu_cesiones times and then sleeps ds2_espera_gpu_us: a Horizon sleep often lasts
// far longer than asked, so short waits (most) do not pay for one. The same hook as ds3-nx's (ds3_D3D_GpuWaitStep) and
// gears2-nx's (sub_82977688), same function.
REXCVAR_DEFINE_INT32(ds2_espera_gpu_us, 100, "DS2",
                     "Microsegundos que duerme la espera activa de Direct3D a la GPU (D3D::CBlocker::Check) tras las "
                     "cesiones; 0 = girar como en la Xbox 360")
    .range(0, 2000);
REXCVAR_DEFINE_INT32(ds2_espera_gpu_cesiones, 32, "DS2",
                     "Veces que la espera de Direct3D a la GPU solo cede el nucleo antes de dormir")
    .range(0, 100000);

REX_EXTERN(__imp__D3D__CBlocker__Check);
REX_EXTERN(D3D__CBlocker__Check) {
  thread_local uint32_t seguidas = 0;  // "still waiting" answers in a row on this thread
  __imp__D3D__CBlocker__Check(ctx, base);
  if (ctx.r3.u32 != 1) {  // the GPU got there
    seguidas = 0;
    return;
  }
  const int32_t us = REXCVAR_GET(ds2_espera_gpu_us);
  if (us <= 0) return;
  if (++seguidas <= uint32_t(REXCVAR_GET(ds2_espera_gpu_cesiones))) {
    rex::thread::MaybeYield();
  } else {
    rex::thread::Sleep(std::chrono::microseconds(us));
  }
}

namespace ds2::cpu {

void InformarSistema() {
#if defined(__SWITCH__)
  const u32 hos = hosversionGet();
  const bool aplicacion = appletGetAppletType() == AppletType_Application;
  REXLOG_INFO("ds2-nx {} (commit {}), firmware {}.{}.{}{}, {}, {}", DS2_VERSION, DS2_COMMIT, HOSVER_MAJOR(hos),
              HOSVER_MINOR(hos), HOSVER_MICRO(hos), hosversionIsAtmosphere() ? " (Atmosphere)" : "",
              aplicacion ? "title takeover" : "APPLET MODE",
              appletGetOperationMode() == AppletOperationMode_Handheld ? "handheld" : "docked");
  if (!aplicacion) {
    REXLOG_WARN("ds2-nx: started as an applet (from the album?): not enough memory for the game. Start any game while "
                "holding R and open the Homebrew Menu from there");
  }
#else
  REXLOG_INFO("ds2-nx {} (commit {})", DS2_VERSION, DS2_COMMIT);
#endif
}

}  // namespace ds2::cpu

// The check build of tools/patch_local_regs.py (DS2_LOCAL_REGS=CHECK, build_mac.sh with PLR_CHECK=1): a call changed
// registers the analysis said it keeps. Logged once per call site (its number, in the build's
// src_tu2_nx/patch_local_regs.check.map), at most 256 sites, with each register that changed (before -> after). A
// poisoned value a callee read shows up as a fault at an address whose low word is the site's number instead (the
// register holds 0x5EAD5EAD:<site>).
extern "C" void ds2_regcheck_fail(const char* function, const char* callee, int site, const char* regs,
                                  const uint64_t* before, const PPCContext* ctx) {
  static std::mutex mutex;
  static std::unordered_set<int> seen;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.size() >= 256 || !seen.insert(site).second) return;
  }
  static const PPCRegister PPCContext::*const kR[32] = {
      &PPCContext::r0,  &PPCContext::r1,  &PPCContext::r2,  &PPCContext::r3,  &PPCContext::r4,  &PPCContext::r5,
      &PPCContext::r6,  &PPCContext::r7,  &PPCContext::r8,  &PPCContext::r9,  &PPCContext::r10, &PPCContext::r11,
      &PPCContext::r12, &PPCContext::r13, &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17,
      &PPCContext::r18, &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
      &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29,
      &PPCContext::r30, &PPCContext::r31};
  static const PPCRegister PPCContext::*const kF[32] = {
      &PPCContext::f0,  &PPCContext::f1,  &PPCContext::f2,  &PPCContext::f3,  &PPCContext::f4,  &PPCContext::f5,
      &PPCContext::f6,  &PPCContext::f7,  &PPCContext::f8,  &PPCContext::f9,  &PPCContext::f10, &PPCContext::f11,
      &PPCContext::f12, &PPCContext::f13, &PPCContext::f14, &PPCContext::f15, &PPCContext::f16, &PPCContext::f17,
      &PPCContext::f18, &PPCContext::f19, &PPCContext::f20, &PPCContext::f21, &PPCContext::f22, &PPCContext::f23,
      &PPCContext::f24, &PPCContext::f25, &PPCContext::f26, &PPCContext::f27, &PPCContext::f28, &PPCContext::f29,
      &PPCContext::f30, &PPCContext::f31};
  std::string changed;
  const char* p = regs;
  for (int k = 0; *p; ++k) {
    const char kind = *p++;
    int n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    while (*p == ' ') ++p;
    const uint64_t now = (ctx->*(kind == 'r' ? kR : kF)[n & 31]).u64;
    if (now != before[k]) changed += fmt::format(" {}{} {:X}->{:X}", kind, n, before[k], now);
  }
  REXLOG_WARN("[regcheck] site {:06X} ({}): the call to {} changed{}", site, function, callee, changed);
}
