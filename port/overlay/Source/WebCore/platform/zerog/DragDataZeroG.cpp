// What is dragged over a ZeroG page: the drag's items (PasteboardItemsZeroG.h),
// files from the host included. Follows the GLib port's (DragDataGLib.cpp).

#include "config.h"
#include "DragData.h"

#if ENABLE(DRAG_SUPPORT)

#include "Color.h"
#include "PasteboardItemsZeroG.h"

namespace WebCore {

static PasteboardItemsZeroG* items(DragDataRef data)
{
    return static_cast<PasteboardItemsZeroG*>(data);
}

static String itemString(DragDataRef data, ASCIILiteral type)
{
    auto* dragItems = items(data);
    auto buffer = dragItems ? dragItems->read(type) : nullptr;
    return buffer ? String::fromUTF8(buffer->span()) : String();
}

bool DragData::canSmartReplace() const
{
    return false;
}

bool DragData::containsColor() const
{
    return false;
}

bool DragData::containsFiles() const
{
    return numberOfFiles();
}

unsigned DragData::numberOfFiles() const
{
    auto* dragItems = items(m_platformDragData);
    return !m_disallowFileAccess && dragItems ? dragItems->filePaths().size() : 0;
}

Vector<String> DragData::asFilenames() const
{
    auto* dragItems = items(m_platformDragData);
    if (m_disallowFileAccess || !dragItems)
        return { };
    return dragItems->filePaths();
}

bool DragData::containsPlainText() const
{
    return !itemString(m_platformDragData, "text/plain"_s).isEmpty();
}

String DragData::asPlainText() const
{
    return itemString(m_platformDragData, "text/plain"_s);
}

Color DragData::asColor() const
{
    return Color();
}

bool DragData::containsCompatibleContent(DraggingPurpose) const
{
    return containsPlainText() || containsURL() || !itemString(m_platformDragData, "text/html"_s).isEmpty() || containsFiles();
}

bool DragData::containsURL(FilenameConversionPolicy filenamePolicy) const
{
    return !asURL(filenamePolicy).isEmpty();
}

String DragData::asURL(FilenameConversionPolicy filenamePolicy, String* title) const
{
    // The first URL of the list.
    for (auto line : StringView { itemString(m_platformDragData, "text/uri-list"_s) }.split('\n')) {
        auto trimmed = line.trim(isASCIIWhitespace<char16_t>);
        if (trimmed.isEmpty() || trimmed.startsWith('#'))
            continue;
        URL url { trimmed.toString() };
        if (filenamePolicy != ConvertFilenames && url.protocolIsFile())
            return { };
        if (title)
            *title = { };
        return url.string();
    }
    return { };
}

bool DragData::shouldMatchStyleOnDrop() const
{
    return false;
}

} // namespace WebCore

#endif // ENABLE(DRAG_SUPPORT)
