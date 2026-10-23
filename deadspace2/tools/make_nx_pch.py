#!/usr/bin/env python3
"""Make the Switch PCH for Dead Space 2's checked-in recompiled tree.

src_tu2/ds2_rex_diagnostic_pch.h was rendered from the DS2 fork's pch_h.inja (ReXGlue
v0.10.0 + rexglue_ds2.patch). rexglue-sdk-nx renders the same template with two changes
that devkitA64 needs, and DS2's codegen emits one macro the nx template lacks:

  1. DEFINE_REX_FUNC gets a GCC branch (attributes after the declarator). Without it GCC
     drops the weak alias and the link fails with every sub_ symbol undefined.
  2. REX_RAW_ADDR_W(x, n): DS2's codegen writes it for stores; outside the official-SDK
     NX memory model it is REX_RAW_ADDR(x).

Usage: make_nx_pch.py <src_tu2/ds2_rex_diagnostic_pch.h> <out.h>
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()

GCC_BRANCH = '''#elif defined(__GNUC__) && !defined(__clang__)
// GCC (devkitA64): attributes go after the declarator, or the weak alias is dropped.
#define DEFINE_REX_FUNC(name)                                       \\
  extern "C" REX_FUNC(name)                                         \\
      __attribute__((alias("__imp__" #name), weak, noinline));      \\
  REX_EXTERN(__imp__##name)
#else
#define DEFINE_REX_FUNC(name)                                       \\
  __attribute__((alias("__imp__" #name)))'''

anchor = '''#else
#define DEFINE_REX_FUNC(name)                                       \\
  __attribute__((alias("__imp__" #name)))'''
if text.count(anchor) != 1:
    sys.exit("DEFINE_REX_FUNC anchor not found exactly once")
text = text.replace(anchor, GCC_BRANCH)

if "REX_RAW_ADDR_W" not in text:
    text = text.rstrip() + '''

//=============================================================================
// Switch (rexglue-sdk-nx): DS2's codegen emits REX_RAW_ADDR_W for stores.
//=============================================================================
#ifndef REX_RAW_ADDR_W
#define REX_RAW_ADDR_W(x, n) REX_RAW_ADDR(x)
#endif
'''
# 3. The interrupt locks. Guest code brackets its lwarx/stwcx. sequences with mtmsrd (interrupts off), and the codegen
#    turns each of those into one process-wide recursive mutex. The atomic itself is already a host compare-and-swap;
#    the mutex only serialized every thread doing atomics: Dead Space 2's job scheduler (the main thread queuing
#    skinning and culling jobs, four EARSJob workers taking them) spent up to a quarter of the main thread blocked on
#    it with 3 cores for 9 busy threads. Now the "interrupts off" state is per thread (what mfmsr reads back), and the
#    old global lock stays available (ppc_msr_cerrojo_global_(), set at startup by ds2_cpu.cpp from a cvar).
OLD_LOCKS_START = "#define REX_CHECK_GLOBAL_LOCK()"
OLD_LOCKS_END = "rex::thread::global_critical_region::mutex().unlock();                                \\\n  } while (0)\n"
i = text.find(OLD_LOCKS_START)
j = text.find(OLD_LOCKS_END, i)
if i < 0 or j < 0 or text.count(OLD_LOCKS_START) != 1:
    sys.exit("interrupt lock macros not found exactly once")
j += len(OLD_LOCKS_END)
NEW_LOCKS = """// Switch (make_nx_pch.py): the interrupt locks per thread unless ppc_msr_cerrojo_global_() (see the script).
inline bool& ppc_msr_cerrojo_global_() {
  static bool global = false;
  return global;
}

inline i32& ppc_msr_profundidad_() {
  static thread_local i32 profundidad = 0;
  return profundidad;
}

#define REX_CHECK_GLOBAL_LOCK()                                          \\
  (ppc_msr_cerrojo_global_() ? ([&]() -> u64 {                           \\
    auto lock_ = rex::thread::global_critical_region::AcquireDirect();   \\
    return ppc_global_lock_count_().load() ? 0 : 0x8000;                 \\
  }())                                                                   \\
                             : (ppc_msr_profundidad_() ? u64(0) : u64(0x8000)))

#define REX_ENTER_GLOBAL_LOCK()                              \\
  do {                                                       \\
    if (ppc_msr_cerrojo_global_()) {                         \\
      rex::thread::global_critical_region::mutex().lock();   \\
      ppc_global_lock_count_().fetch_add(1);                 \\
    } else {                                                 \\
      ++ppc_msr_profundidad_();                              \\
    }                                                        \\
  } while (0)

#define REX_LEAVE_GLOBAL_LOCK()                              \\
  do {                                                       \\
    if (ppc_msr_cerrojo_global_()) {                         \\
      ppc_global_lock_count_().fetch_sub(1);                 \\
      rex::thread::global_critical_region::mutex().unlock(); \\
    } else {                                                 \\
      --ppc_msr_profundidad_();                              \\
    }                                                        \\
  } while (0)
"""
text = text[:i] + NEW_LOCKS + text[j:]

# 4. The check build of tools/patch_local_regs.py (DS2_LOCAL_REGS=CHECK): its calls report a register a callee changed
#    though the analysis said it keeps it (defined in src/ds2_cpu.cpp).
if "ds2_regcheck_fail" not in text:
    text = text.rstrip() + '''

//=============================================================================
// Switch (make_nx_pch.py): the check build of tools/patch_local_regs.py.
//=============================================================================
extern "C" void ds2_regcheck_fail(const char* function, const char* callee, int site, const char* regs,
                                  const uint64_t* before, const PPCContext* ctx);
'''

# Only when it changes: every unit of the recompiled tree depends on this header through the PCH, so rewriting it
# with the same text on each configure made the next build recompile all of them (~11 minutes on a Mac).
try:
    previous = open(dst, encoding="utf-8").read()
except OSError:
    previous = None
if previous == text:
    print("unchanged", dst)
else:
    open(dst, "w", encoding="utf-8").write(text)
    print("wrote", dst)
