// nfsmw - automated tests on the PC: a virtual pad driven by a button script.
//
// With nfsmw_prueba_botones (for example "92:start,98:start,130-160:rt") the app
// adds to the SDK input system a synthetic device that presses those buttons at
// those seconds after the input system is created. It reaches the menus with a
// 3D scene and a race unattended.
//
// InputSystem::GetState merges all devices of user 0 and the synthetic ones go
// to that user (SlotAssignment), so it coexists with the real pad and keyboard.
// Empty (the default) adds nothing.

#pragma once

#include <rex/runtime.h>

namespace nfsmw::prueba {

// Wraps config.input_factory to add the script pad if the cvar is not empty. Call it in
// OnPreSetup, once ReXApp has installed the default factory.
void EnvolverEntrada(rex::RuntimeConfig& config);

}  // namespace nfsmw::prueba
