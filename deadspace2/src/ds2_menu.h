// deadspace2 - the port's options menu (ZL + ZR + Minus): see ds2_menu.cpp.

#pragma once

namespace rex {
class Runtime;
}

namespace ds2::menu {

// Takes the controller filter and shows the menu when asked; call once the runtime exists.
void Instalar(rex::Runtime& runtime);

}  // namespace ds2::menu
