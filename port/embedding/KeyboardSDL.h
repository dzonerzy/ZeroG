#pragma once

#include "View.h"

namespace ZeroG {

View::Modifiers modifiersFromSDL(unsigned sdlModifiers);
View::KeyEvent keyEventFromSDL(unsigned scancode, unsigned sdlModifiers, bool repeat);

} // namespace ZeroG
