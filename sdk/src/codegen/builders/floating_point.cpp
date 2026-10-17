/**
 * @file        rexcodegen/builders/floating_point.cpp
 * @brief       PPC floating point instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

namespace rex::codegen {

//=============================================================================
// Sign Manipulation
//=============================================================================

bool build_fabs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.u64 = {}.u64 & ~0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fnabs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.u64 = {}.u64 | 0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fneg(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.u64 = {}.u64 ^ 0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Move and Conversion
//=============================================================================

bool build_fmr(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64;", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fcfid(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double({}.s64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctid(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  // double(LLONG_MAX) rounds up to exactly 2^63, which is not representable as
  // an int64_t, so the saturation test must be >= rather than >: an input of
  // exactly 2^63 would otherwise fall through to the host conversion and yield
  // the "integer indefinite" value 0x8000000000000000 - a large negative result
  // for a large positive input.
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x8000000000000000ULL) : "
      "({1}.f64 >= double(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvtsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctidz(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  // See build_fctid for why the saturation test is >= and not >.
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x8000000000000000ULL) : "
      "({1}.f64 >= double(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvttsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctiw(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x80000000U) : "
      "({1}.f64 >= double(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvtsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctiwz(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x80000000U) : "
      "({1}.f64 >= double(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvttsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_frsp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Comparison
//=============================================================================

bool build_fcmpu(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.compare({}.f64, {}.f64);", ctx.cr(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fcmpo(BuilderContext& ctx) {
  // fcmpo is identical to fcmpu for recompilation purposes.
  // The difference is that fcmpo sets FPSCR exception flags for SNaN operands,
  // which we don't need to emulate.
  return build_fcmpu(ctx);
}

//=============================================================================
// Addition
//=============================================================================

bool build_fadd(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 + {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fadds(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 + {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Subtraction
//=============================================================================

bool build_fsub(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 - {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fsubs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 - {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Multiplication
//=============================================================================

bool build_fmul(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 * {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fmuls(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 * {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Division
//=============================================================================

bool build_fdiv(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 / {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fdivs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 / {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Fused Multiply-Add
//=============================================================================

bool build_fmadd(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = std::fma({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmadds(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(std::fma({}.f64, {}.f64, {}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmsub(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = std::fma({}.f64, {}.f64, -{}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmsubs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(std::fma({}.f64, {}.f64, -{}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmadd(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = -std::fma({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmadds(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(-std::fma({}.f64, {}.f64, {}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmsub(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = -std::fma({}.f64, {}.f64, -{}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmsubs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(-std::fma({}.f64, {}.f64, -{}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

//=============================================================================
// Reciprocal and Square Root
//=============================================================================

bool build_fres(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  // Narrow the INPUT to single precision before taking the reciprocal, matching
  // xenia (Recip(Convert(frB, FLOAT32_TYPE))). Computing 1.0/frB in double and
  // rounding afterwards gives a different result in the low bits.
  ctx.println("\t{}.f64 = double(1.0f / float({}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_frsqrte(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  // NOTE: xenia keeps this in full double precision (RSqrt(LoadFPR(frB)), no
  // ToSingle) and its x64 backend implements the real hardware estimate via a
  // lookup table (X64HelperEmitter::EmitFrsqrteHelper), including the non-IEEE
  // mode and NaN/denormal edge cases. This is a plain double-precision
  // approximation instead - more accurate than the ~5-bit hardware estimate,
  // but not bit-exact with either the console or xenia. Left as-is: unlike fres
  // there is no cheap expression that gets closer.
  ctx.println("\t{}.f64 = 1.0 / sqrt({}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fsqrt(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = sqrt({}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fsqrts(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(sqrt({}.f64)));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Selection
//=============================================================================

bool build_fsel(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 >= 0.0 ? {}.f64 : {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

}  // namespace rex::codegen
