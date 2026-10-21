// deadspace2 - the recompiled code's "interrupts off" regions (ds2_cpu.cpp).

#pragma once

namespace ds2::cpu {

// Before the game's first thread runs: reads ds2_msr_cerrojo_global.
void Configurar();

// The build (version, commit) and the console (firmware, Atmosphere, title takeover or applet, handheld or docked) at
// the top of the log: what a player's log needs to say before anything else.
void InformarSistema();

}  // namespace ds2::cpu
