#include "config.h"
#include "PlatformKeyboardEvent.h"

namespace WebCore {

// ZeroG delivers key events as KeyDown with both the text and the key identifier
// filled in; WebCore splits them into RawKeyDown (no text) and Char (no key
// identifier). Text committed by an input method keeps both.
void PlatformKeyboardEvent::disambiguateKeyDownEvent(Type type, bool backwardCompatibilityMode)
{
    ASSERT(m_type == PlatformEvent::Type::KeyDown);
    ASSERT(type == Type::RawKeyDown || type == Type::Char);
    m_type = type;

    if (backwardCompatibilityMode || m_handledByInputMethod)
        return;

    if (type == PlatformEvent::Type::RawKeyDown) {
        m_text = String();
        m_unmodifiedText = String();
    } else
        m_keyIdentifier = String();
}

} // namespace WebCore
