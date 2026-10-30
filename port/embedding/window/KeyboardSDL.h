#pragma once

#include "View.h"

namespace ZeroG {

View::Modifiers modifiersFromSDL(unsigned sdlModifiers);
View::KeyEvent keyEventFromSDL(unsigned scancode, unsigned sdlModifiers, bool repeat);
// The Windows virtual key code of a DOM KeyboardEvent.code ("KeyA", "Enter"), 0 if
// unknown: for key events the application describes with DOM strings.
int windowsKeyCodeForDOMCode(const String&);

} // namespace ZeroG
