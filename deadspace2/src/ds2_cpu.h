// deadspace2 - the recompiled code's "interrupts off" regions (ds2_cpu.cpp).

#pragma once

namespace ds2::cpu {

// Before the game's first thread runs: reads ds2_msr_cerrojo_global.
void Configurar();

}  // namespace ds2::cpu
