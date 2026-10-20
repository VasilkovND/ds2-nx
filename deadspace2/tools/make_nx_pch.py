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
