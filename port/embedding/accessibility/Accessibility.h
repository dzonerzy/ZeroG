#pragma once

// Accessibility (docs/design/input-ime-clipboard-accessibility.md, "Accessibility"): WebCore's accessibility tree as the
// OS bridges read it (AccessibilityUIA.cpp on Windows, AccessibilityAtspi.cpp on
// Linux). Engine thread only. Objects are WebCore's AXCoreObjects, kept across
// calls by their AXID; a bridge looks them up again each time (object()), since
// WebCore may drop them at any layout.

#include <WebCore/AXCoreObject.h>
#include <WebCore/AXID.h>
#include <WebCore/FloatRect.h>
#include <WebCore/IntPoint.h>
#include <memory>
#include <optional>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace ZeroG {

class View;

namespace Accessibility {

// What an object is, in the terms both UI Automation and AT-SPI have.
enum class Role : uint8_t {
    Unknown,
    Document, // a web area
    Group,
    Text, // static text
    Paragraph,
    Heading,
    Link,
    Button,
    ToggleButton,
    CheckBox,
    RadioButton,
    RadioGroup,
    Switch,
    TextField,
    PasswordField,
    TextArea,
    ComboBox,
    ListBox,
    ListBoxOption,
    List,
    ListItem,
    Image,
    Table,
    Row,
    Cell,
    ColumnHeader,
    RowHeader,
    Slider,
    SpinButton,
    ProgressBar,
    Menu,
    MenuBar,
    MenuItem,
    TabList,
    Tab,
    TabPanel,
    Dialog,
    Alert,
    Toolbar,
    Tree,
    TreeItem,
    Separator,
    Landmark,
    Form,
    ScrollBar,
    Label,
    Figure,
    Article,
    Status,
};

enum class State : uint32_t {
    Enabled = 1 << 0,
    Focusable = 1 << 1,
    Focused = 1 << 2,
    Checkable = 1 << 3,
    Checked = 1 << 4,
    Mixed = 1 << 5, // checked: neither on nor off
    Selected = 1 << 6,
    Expandable = 1 << 7,
    Expanded = 1 << 8,
    Pressed = 1 << 9,
    Editable = 1 << 10,
    ReadOnly = 1 << 11,
    Required = 1 << 12,
    Password = 1 << 13,
    MultiLine = 1 << 14,
    Visible = 1 << 15,
    Offscreen = 1 << 16,
};

struct RangeValue {
    double current { 0 };
    double minimum { 0 };
    double maximum { 0 };
};

using Object = WebCore::AXCoreObject;

// The view's web area (the main frame's document), with accessibility turned on
// in WebCore the first time; null before a document has a renderer.
Object* root(View&);
// The object of a view with this ID, or null once it is gone.
Object* object(View&, WebCore::AXID);
// The object with the keyboard focus, if it is in the view.
Object* focused(View&);
// The deepest object at a point of the view (logical pixels).
Object* hitTest(View&, const WebCore::IntPoint&);

Role role(const Object&);
String name(const Object&);
String description(const Object&);
// What a text field, combo box or list option holds, or a link's URL.
String value(const Object&);
// Sliders, spin buttons, progress bars.
std::optional<RangeValue> rangeValue(const Object&);
OptionSet<State> states(const Object&);
// In the view, logical pixels.
WebCore::FloatRect bounds(const Object&);
// The objects the tree exposes (ignored ones skipped), and the parent; the root
// has none.
Vector<Ref<Object>> children(Object&);
Object* parent(const Object&, View&);

// What doDefaultAction does, for the OS's action lists ("press", "click",
// "toggle", "select", "jump"); empty: nothing.
String defaultActionName(const Object&);
bool doDefaultAction(Object&);
// The text assistive technologies read in an object: an editable field's
// value, otherwise its name.
String text(const Object&);
// A text field's or text area's selection (UTF-16 offsets into text(); the
// caret when empty), and selecting in it. nullopt / false elsewhere.
struct TextSelection {
    unsigned start { 0 };
    unsigned end { 0 };
};
std::optional<TextSelection> textSelection(const Object&);
bool setTextSelection(Object&, TextSelection);

// Text fields and combo boxes take text; ranges take numbers.
bool canSetValue(const Object&);
bool setValue(Object&, const String&);
bool setRangeValue(Object&, double);
bool setFocus(Object&);

// What assistive technologies hear about. Events are queued and delivered after
// WebCore's notification returns, on the engine thread, so a bridge may read
// the tree (and the OS may call back) while it handles them.
enum class Event : uint8_t {
    FocusChanged,
    ChildrenChanged,
    NameChanged,
    ValueChanged,
    StateChanged,
    LoadComplete,
};

// One per window, created by the OS bridge for the window's view.
class Bridge {
public:
    virtual ~Bridge();
    View& view() const { return m_view; }
    virtual void event(Object&, Event) = 0;
    // An object is gone: references the OS holds become defunct.
    virtual void objectDetached(WebCore::AXID) = 0;
    // The window's title (the name of its element where the OS does not take it
    // from the window itself).
    virtual void titleChanged(const String&) { }

protected:
    explicit Bridge(View&);

private:
    View& m_view;
};

// The window's OS bridge: on Windows `nativeWindow` is its HWND (UI Automation
// asks it with WM_GETOBJECT); on Linux the application joins the AT-SPI bus
// once, and each window is one of its children, named `title`. Null when the
// OS has no accessibility service to talk to.
std::unique_ptr<Bridge> createBridge(View&, void* nativeWindow, const String& title);

} // namespace Accessibility
} // namespace ZeroG
