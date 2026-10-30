#include "config.h"
#include "accessibility/Accessibility.h"

#include "View.h"
#include <WebCore/AXObjectCache.h>
#include <WebCore/AccessibilityClientZeroG.h>
#include <WebCore/AccessibilityObject.h>
#include <WebCore/Document.h>
#include <WebCore/DocumentView.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/Page.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>

namespace ZeroG::Accessibility {

using namespace WebCore;

static AXObjectCache* cacheFor(View& view)
{
    RefPtr frame = view.page().localMainFrame();
    RefPtr document = frame ? frame->document() : nullptr;
    if (!document)
        return nullptr;
    // WebCore builds no tree until something asks for one.
    if (!AXObjectCache::accessibilityEnabled())
        AXObjectCache::enableAccessibility();
    return document->axObjectCache();
}

Object* root(View& view)
{
    auto* cache = cacheFor(view);
    RefPtr frame = view.page().localMainFrame();
    if (!cache || !frame)
        return nullptr;
    // The frame's scroll view, whose child is the document's web area.
    auto* scrollView = cache->rootObjectForFrame(*frame);
    if (!scrollView)
        return nullptr;
    for (auto& child : scrollView->unignoredChildren()) {
        if (child->isWebArea())
            return child.ptr();
    }
    return nullptr;
}

Object* object(View& view, AXID id)
{
    auto* cache = cacheFor(view);
    auto* object = cache ? cache->objectForID(id) : nullptr;
    return object && !object->isDetached() ? object : nullptr;
}

Object* focused(View& view)
{
    auto* cache = cacheFor(view);
    return cache ? cache->focusedObjectForPage(&view.page()) : nullptr;
}

Object* hitTest(View& view, const IntPoint& point)
{
    auto* cache = cacheFor(view);
    RefPtr frame = view.page().localMainFrame();
    RefPtr frameView = frame ? frame->view() : nullptr;
    if (!cache || !frameView)
        return nullptr;
    auto* scrollView = cache->rootObjectForFrame(*frame);
    if (!scrollView)
        return nullptr;
    // Hit tests take document coordinates, as elementRect().
    RefPtr hit = scrollView->accessibilityHitTest(frameView->rootViewToContents(point));
    return hit.get();
}

Role role(const Object& object)
{
    switch (object.role()) {
    case AccessibilityRole::WebArea:
    case AccessibilityRole::Document:
    case AccessibilityRole::GraphicsDocument:
        return Role::Document;
    case AccessibilityRole::StaticText:
    case AccessibilityRole::Time:
        return Role::Text;
    case AccessibilityRole::Paragraph:
        return Role::Paragraph;
    case AccessibilityRole::Heading:
        return Role::Heading;
    case AccessibilityRole::Link:
        return Role::Link;
    case AccessibilityRole::Button:
    case AccessibilityRole::PopUpButton:
        return Role::Button;
    case AccessibilityRole::ToggleButton:
        return Role::ToggleButton;
    case AccessibilityRole::Checkbox:
    case AccessibilityRole::MenuItemCheckbox:
        return Role::CheckBox;
    case AccessibilityRole::RadioButton:
    case AccessibilityRole::MenuItemRadio:
        return Role::RadioButton;
    case AccessibilityRole::RadioGroup:
        return Role::RadioGroup;
    case AccessibilityRole::Switch:
        return Role::Switch;
    case AccessibilityRole::TextField:
    case AccessibilityRole::SearchField:
        return object.isSecureField() ? Role::PasswordField : Role::TextField;
    case AccessibilityRole::TextArea:
        return Role::TextArea;
    case AccessibilityRole::ComboBox:
        return Role::ComboBox;
    case AccessibilityRole::ListBox:
    case AccessibilityRole::MenuListPopup:
        return Role::ListBox;
    case AccessibilityRole::ListBoxOption:
    case AccessibilityRole::MenuListOption:
        return Role::ListBoxOption;
    case AccessibilityRole::List:
    case AccessibilityRole::DescriptionList:
    case AccessibilityRole::Directory:
        return Role::List;
    case AccessibilityRole::ListItem:
    case AccessibilityRole::DescriptionListTerm:
    case AccessibilityRole::DescriptionListDetail:
        return Role::ListItem;
    case AccessibilityRole::Image:
    case AccessibilityRole::ImageMap:
    case AccessibilityRole::Canvas:
    case AccessibilityRole::SVGRoot:
        return Role::Image;
    case AccessibilityRole::Table:
    case AccessibilityRole::Grid:
    case AccessibilityRole::TreeGrid:
        return Role::Table;
    case AccessibilityRole::Row:
        return Role::Row;
    case AccessibilityRole::Cell:
    case AccessibilityRole::GridCell:
        return Role::Cell;
    case AccessibilityRole::ColumnHeader:
        return Role::ColumnHeader;
    case AccessibilityRole::RowHeader:
        return Role::RowHeader;
    case AccessibilityRole::Slider:
        return Role::Slider;
    case AccessibilityRole::SpinButton:
        return Role::SpinButton;
    case AccessibilityRole::ProgressIndicator:
    case AccessibilityRole::Meter:
        return Role::ProgressBar;
    case AccessibilityRole::Menu:
        return Role::Menu;
    case AccessibilityRole::MenuBar:
        return Role::MenuBar;
    case AccessibilityRole::MenuItem:
        return Role::MenuItem;
    case AccessibilityRole::TabList:
        return Role::TabList;
    case AccessibilityRole::Tab:
        return Role::Tab;
    case AccessibilityRole::TabPanel:
        return Role::TabPanel;
    case AccessibilityRole::ApplicationDialog:
    case AccessibilityRole::ApplicationAlertDialog:
        return Role::Dialog;
    case AccessibilityRole::ApplicationAlert:
        return Role::Alert;
    case AccessibilityRole::Toolbar:
        return Role::Toolbar;
    case AccessibilityRole::Tree:
        return Role::Tree;
    case AccessibilityRole::TreeItem:
        return Role::TreeItem;
    case AccessibilityRole::HorizontalRule:
    case AccessibilityRole::Splitter:
        return Role::Separator;
    case AccessibilityRole::LandmarkBanner:
    case AccessibilityRole::LandmarkComplementary:
    case AccessibilityRole::LandmarkContentInfo:
    case AccessibilityRole::LandmarkDocRegion:
    case AccessibilityRole::LandmarkMain:
    case AccessibilityRole::LandmarkNavigation:
    case AccessibilityRole::LandmarkRegion:
    case AccessibilityRole::LandmarkSearch:
        return Role::Landmark;
    case AccessibilityRole::Form:
        return Role::Form;
    case AccessibilityRole::ScrollBar:
        return Role::ScrollBar;
    case AccessibilityRole::Label:
    case AccessibilityRole::Legend:
        return Role::Label;
    case AccessibilityRole::Figure:
        return Role::Figure;
    case AccessibilityRole::DocumentArticle:
    case AccessibilityRole::Feed:
        return Role::Article;
    case AccessibilityRole::ApplicationStatus:
    case AccessibilityRole::ApplicationLog:
    case AccessibilityRole::ApplicationTimer:
    case AccessibilityRole::ApplicationMarquee:
        return Role::Status;
    case AccessibilityRole::Unknown:
        return Role::Unknown;
    default:
        return Role::Group;
    }
}

// The name, then the description: the first of WebCore's text alternatives
// (in the order the accname spec computes them) that is not a help text or a
// summary, which are the description. As upstream's AT-SPI bridge
// (AccessibilityObjectAtspi.cpp) does.
String name(const Object& object)
{
    if (object.role() == AccessibilityRole::ListBoxOption || object.role() == AccessibilityRole::MenuListOption) {
        auto value = object.stringValue();
        if (!value.isEmpty())
            return value;
    }
    Vector<AccessibilityText> texts;
    object.accessibilityText(texts);
    for (auto& text : texts) {
        if (text.text.isEmpty())
            continue;
        if (text.textSource != AccessibilityTextSource::Help && text.textSource != AccessibilityTextSource::Summary)
            return text.text;
    }
    // Static text is its own name.
    if (object.role() == AccessibilityRole::StaticText)
        return object.stringValue();
    return emptyString();
}

String description(const Object& object)
{
    Vector<AccessibilityText> texts;
    object.accessibilityText(texts);
    bool hasName = false;
    for (auto& text : texts) {
        if (text.textSource == AccessibilityTextSource::Help || text.textSource == AccessibilityTextSource::Summary)
            return text.text;
        // A title with no other alternative is the name, not also the description.
        if (text.textSource == AccessibilityTextSource::TitleTag && hasName)
            return text.text;
        hasName = true;
    }
    return emptyString();
}

String value(const Object& object)
{
    switch (role(object)) {
    case Role::PasswordField:
        return emptyString(); // never told
    case Role::Link:
        return object.url().string();
    default:
        return object.stringValue();
    }
}

std::optional<RangeValue> rangeValue(const Object& object)
{
    switch (role(object)) {
    case Role::Slider:
    case Role::SpinButton:
    case Role::ProgressBar:
    case Role::ScrollBar:
        return RangeValue { object.valueForRange(), object.minValueForRange(), object.maxValueForRange() };
    default:
        return std::nullopt;
    }
}

OptionSet<State> states(const Object& object)
{
    OptionSet<State> states;
    if (object.isEnabled())
        states.add(State::Enabled);
    if (object.canSetFocusAttribute())
        states.add(State::Focusable);
    if (object.isFocused())
        states.add(State::Focused);
    if (object.supportsChecked()) {
        states.add(State::Checkable);
        switch (object.checkboxOrRadioValue()) {
        case AccessibilityButtonState::On:
            states.add(State::Checked);
            break;
        case AccessibilityButtonState::Mixed:
            states.add(State::Mixed);
            break;
        case AccessibilityButtonState::Off:
            break;
        }
    }
    if (object.isSelected())
        states.add(State::Selected);
    if (object.supportsExpanded()) {
        states.add(State::Expandable);
        if (object.isExpanded())
            states.add(State::Expanded);
    }
    if (object.isPressed())
        states.add(State::Pressed);
    if (object.isTextControl() || object.isNonNativeTextControl()) {
        states.add(State::Editable);
        if (!object.canSetValueAttribute())
            states.add(State::ReadOnly);
    }
    if (object.isRequired())
        states.add(State::Required);
    if (object.isSecureField())
        states.add(State::Password);
    if (object.role() == AccessibilityRole::TextArea)
        states.add(State::MultiLine);
    if (object.isVisible())
        states.add(State::Visible);
    if (object.isOffScreen())
        states.add(State::Offscreen);
    return states;
}

FloatRect bounds(const Object& object)
{
    return object.relativeFrame();
}

Vector<Ref<Object>> children(Object& object)
{
    Vector<Ref<Object>> result;
    for (auto& child : object.unignoredChildren())
        result.append(child.get());
    return result;
}

Object* parent(const Object& object, View& view)
{
    if (&object == root(view))
        return nullptr;
    return object.parentObjectUnignored();
}

String defaultActionName(const Object& object)
{
    if (!object.supportsPressAction())
        return { };
    switch (role(object)) {
    case Role::CheckBox:
    case Role::Switch:
    case Role::ToggleButton:
        return "toggle"_s;
    case Role::RadioButton:
    case Role::ListBoxOption:
    case Role::Tab:
        return "select"_s;
    case Role::Link:
        return "jump"_s;
    case Role::Button:
    case Role::MenuItem:
        return "press"_s;
    default:
        return "click"_s;
    }
}

bool doDefaultAction(Object& object)
{
    return object.press();
}

bool canSetValue(const Object& object)
{
    if (!object.canSetValueAttribute())
        return false;
    switch (role(object)) {
    case Role::TextField:
    case Role::PasswordField:
    case Role::TextArea:
    case Role::ComboBox:
    case Role::Slider:
    case Role::SpinButton:
        return true;
    default:
        return object.isNonNativeTextControl();
    }
}

bool setValue(Object& object, const String& value)
{
    if (!canSetValue(object))
        return false;
    // WebCore sets a text field's value as typing (the editor replaces the
    // selection's text), which needs the field focused, as for whoever types.
    if ((object.isTextControl() || object.isNonNativeTextControl()) && !object.isFocused() && object.canSetFocusAttribute())
        object.setFocused(true);
    return object.setValue(value);
}

bool setRangeValue(Object& object, double value)
{
    return canSetValue(object) && object.setValue(static_cast<float>(value));
}

bool setFocus(Object& object)
{
    if (!object.canSetFocusAttribute())
        return false;
    object.setFocused(true);
    return true;
}

// ---- Events ----

static Vector<Bridge*>& bridges()
{
    static NeverDestroyed<Vector<Bridge*>> bridges;
    return bridges;
}

static Bridge* bridgeFor(const View& view)
{
    for (auto* bridge : bridges()) {
        if (&bridge->view() == &view)
            return bridge;
    }
    return nullptr;
}

namespace {

struct QueuedEvent {
    View* view; // null: a detached object, for every bridge
    AXID id;
    Event event;

    bool operator==(const QueuedEvent&) const = default;
};

// WebCore's notifications, as events queued for the bridges.
class Client final : public AccessibilityClientZeroG {
public:
    void accessibilityEvent(Page& page, AccessibilityObject& object, AccessibilityEventZeroG event) final
    {
        auto* view = View::fromPage(page);
        if (!view || !bridgeFor(*view))
            return;
        enqueue({ view, object.objectID(), static_cast<Event>(event) });
    }

    void accessibilityObjectDetached(AXID id) final
    {
        enqueue({ nullptr, id, Event::ChildrenChanged });
    }

    void clear() { m_queue.clear(); }

private:
    void enqueue(QueuedEvent&& event)
    {
        // The same change several times in a row (typing, a layout) is told once.
        if (m_queue.contains(event))
            return;
        m_queue.append(WTF::move(event));
        if (std::exchange(m_scheduled, true))
            return;
        RunLoop::mainSingleton().dispatch([this] {
            m_scheduled = false;
            deliver();
        });
    }

    void deliver()
    {
        for (auto& queued : std::exchange(m_queue, { })) {
            if (!queued.view) {
                for (auto* bridge : Vector { bridges() })
                    bridge->objectDetached(queued.id);
                continue;
            }
            // The bridge may have gone with its window since.
            auto* bridge = bridgeFor(*queued.view);
            if (!bridge)
                continue;
            if (auto* target = object(*queued.view, queued.id))
                bridge->event(*target, queued.event);
        }
    }

    Vector<QueuedEvent> m_queue;
    bool m_scheduled { false };
};

static_assert(static_cast<uint8_t>(AccessibilityEventZeroG::FocusChanged) == static_cast<uint8_t>(Event::FocusChanged));
static_assert(static_cast<uint8_t>(AccessibilityEventZeroG::ChildrenChanged) == static_cast<uint8_t>(Event::ChildrenChanged));
static_assert(static_cast<uint8_t>(AccessibilityEventZeroG::NameChanged) == static_cast<uint8_t>(Event::NameChanged));
static_assert(static_cast<uint8_t>(AccessibilityEventZeroG::ValueChanged) == static_cast<uint8_t>(Event::ValueChanged));
static_assert(static_cast<uint8_t>(AccessibilityEventZeroG::StateChanged) == static_cast<uint8_t>(Event::StateChanged));
static_assert(static_cast<uint8_t>(AccessibilityEventZeroG::LoadComplete) == static_cast<uint8_t>(Event::LoadComplete));

Client& client()
{
    static NeverDestroyed<Client> client;
    return client;
}

} // namespace

Bridge::Bridge(View& view)
    : m_view(view)
{
    if (bridges().isEmpty())
        setAccessibilityClientZeroG(&client());
    bridges().append(this);
}

Bridge::~Bridge()
{
    bridges().removeFirst(this);
    // Nothing listens any more: WebCore's notifications go nowhere again.
    if (bridges().isEmpty()) {
        setAccessibilityClientZeroG(nullptr);
        client().clear();
    }
}

} // namespace ZeroG::Accessibility
