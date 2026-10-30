// UI Automation for a window (ZeroG.md, "Accessibilità"), written for ZeroG: one
// provider per accessibility object (Accessibility.h), a fragment root for the
// window that answers WM_GETOBJECT, and the patterns screen readers use most:
// Invoke (buttons, links), Toggle (check boxes, switches), Value (text fields,
// combo boxes), RangeValue (sliders, progress bars) and a basic Text pattern
// for text fields and text areas (reading by character, word, line; the
// selection and the caret).
//
// Threading: the providers are created on the engine thread, which is a COM
// single-threaded apartment, and say ProviderOptions_UseComThreading: UI
// Automation calls them there, through the window's message loop (SDL pumps
// every message of the thread while it waits). So they read WebCore's tree
// directly.

#include "config.h"

#if OS(WINDOWS)

#include "accessibility/Accessibility.h"

#include "View.h"
#include <UIAutomation.h>
#include <wrl/client.h>
#include <wtf/HashMap.h>
#include <wtf/MainThread.h>
#include <wtf/text/StringView.h>

using Microsoft::WRL::ComPtr;

namespace ZeroG::Accessibility {

using namespace WebCore;

namespace {

class UIABridge;

BSTR toBSTR(const String& string)
{
    if (string.isEmpty())
        return SysAllocString(L"");
    auto characters = StringView(string).upconvertedCharacters();
    return SysAllocStringLen(reinterpret_cast<const OLECHAR*>(characters.get()), string.length());
}

String fromWide(LPCWSTR text)
{
    if (!text)
        return emptyString();
    return String(std::span<const char16_t>(reinterpret_cast<const char16_t*>(text), wcslen(text)));
}

long controlType(Role role)
{
    switch (role) {
    case Role::Document:
        return UIA_DocumentControlTypeId;
    case Role::Text:
    case Role::Heading:
    case Role::Label:
        return UIA_TextControlTypeId;
    case Role::Link:
        return UIA_HyperlinkControlTypeId;
    case Role::Button:
    case Role::ToggleButton:
        return UIA_ButtonControlTypeId;
    case Role::CheckBox:
    case Role::Switch:
        return UIA_CheckBoxControlTypeId;
    case Role::RadioButton:
        return UIA_RadioButtonControlTypeId;
    case Role::TextField:
    case Role::PasswordField:
    case Role::TextArea:
        return UIA_EditControlTypeId;
    case Role::ComboBox:
        return UIA_ComboBoxControlTypeId;
    case Role::ListBox:
    case Role::List:
        return UIA_ListControlTypeId;
    case Role::ListBoxOption:
    case Role::ListItem:
        return UIA_ListItemControlTypeId;
    case Role::Image:
        return UIA_ImageControlTypeId;
    case Role::Table:
        return UIA_TableControlTypeId;
    case Role::Row:
    case Role::Cell:
        return UIA_DataItemControlTypeId;
    case Role::ColumnHeader:
    case Role::RowHeader:
        return UIA_HeaderItemControlTypeId;
    case Role::Slider:
        return UIA_SliderControlTypeId;
    case Role::SpinButton:
        return UIA_SpinnerControlTypeId;
    case Role::ProgressBar:
        return UIA_ProgressBarControlTypeId;
    case Role::Menu:
        return UIA_MenuControlTypeId;
    case Role::MenuBar:
        return UIA_MenuBarControlTypeId;
    case Role::MenuItem:
        return UIA_MenuItemControlTypeId;
    case Role::TabList:
        return UIA_TabControlTypeId;
    case Role::Tab:
        return UIA_TabItemControlTypeId;
    case Role::Dialog:
    case Role::Alert:
        return UIA_PaneControlTypeId;
    case Role::Toolbar:
        return UIA_ToolBarControlTypeId;
    case Role::Tree:
        return UIA_TreeControlTypeId;
    case Role::TreeItem:
        return UIA_TreeItemControlTypeId;
    case Role::Separator:
        return UIA_SeparatorControlTypeId;
    case Role::ScrollBar:
        return UIA_ScrollBarControlTypeId;
    case Role::Unknown:
        return UIA_CustomControlTypeId;
    default:
        return UIA_GroupControlTypeId;
    }
}

bool isToggle(const Object& object)
{
    auto objectRole = role(object);
    return objectRole == Role::CheckBox || objectRole == Role::Switch || objectRole == Role::ToggleButton;
}

bool hasValuePattern(const Object& object)
{
    switch (role(object)) {
    case Role::TextField:
    case Role::PasswordField:
    case Role::TextArea:
    case Role::ComboBox:
    case Role::Link:
        return true;
    default:
        return object.isNonNativeTextControl();
    }
}

// An accessibility object, or the window's own element (the fragment root, which
// UI Automation merges with the HWND's host provider). Defunct once the object
// or the window is gone: every call then fails with UIA_E_ELEMENTNOTAVAILABLE.
class Provider final
    : public IRawElementProviderSimple
    , public IRawElementProviderFragment
    , public IRawElementProviderFragmentRoot
    , public IInvokeProvider
    , public IToggleProvider
    , public IValueProvider
    , public IRangeValueProvider
    , public ITextProvider {
public:
    Provider(UIABridge& bridge, std::optional<AXID> id)
        : m_bridge(&bridge)
        , m_id(id)
    {
    }

    void disconnect() { m_bridge = nullptr; }
    bool isRoot() const { return !m_id; }
    UIABridge* bridge() const { return m_bridge; }
    // The live object (null for the root, or when defunct).
    Object* object() const;

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID iid, void** result) final
    {
        if (!result)
            return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple))
            *result = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == __uuidof(IRawElementProviderFragment))
            *result = static_cast<IRawElementProviderFragment*>(this);
        else if (iid == __uuidof(IRawElementProviderFragmentRoot) && isRoot())
            *result = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (iid == __uuidof(IInvokeProvider) && !isRoot())
            *result = static_cast<IInvokeProvider*>(this);
        else if (iid == __uuidof(IToggleProvider) && !isRoot())
            *result = static_cast<IToggleProvider*>(this);
        else if (iid == __uuidof(IValueProvider) && !isRoot())
            *result = static_cast<IValueProvider*>(this);
        else if (iid == __uuidof(IRangeValueProvider) && !isRoot())
            *result = static_cast<IRangeValueProvider*>(this);
        else if (iid == __uuidof(ITextProvider) && !isRoot())
            *result = static_cast<ITextProvider*>(this);
        else {
            *result = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() final { return ++m_refCount; }

    IFACEMETHODIMP_(ULONG) Release() final
    {
        auto count = --m_refCount;
        if (!count)
            delete this;
        return count;
    }

    // IRawElementProviderSimple
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* options) final
    {
        if (!options)
            return E_POINTER;
        *options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }

    IFACEMETHODIMP GetPatternProvider(PATTERNID, IUnknown**) final;
    IFACEMETHODIMP GetPropertyValue(PROPERTYID, VARIANT*) final;
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple**) final;

    // IRawElementProviderFragment
    IFACEMETHODIMP Navigate(NavigateDirection, IRawElementProviderFragment**) final;
    IFACEMETHODIMP GetRuntimeId(SAFEARRAY**) final;
    IFACEMETHODIMP get_BoundingRectangle(UiaRect*) final;
    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** roots) final
    {
        if (!roots)
            return E_POINTER;
        *roots = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() final;
    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot**) final;

    // IRawElementProviderFragmentRoot
    IFACEMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment**) final;
    IFACEMETHODIMP GetFocus(IRawElementProviderFragment**) final;

    // IInvokeProvider
    IFACEMETHODIMP Invoke() final;

    // IToggleProvider
    IFACEMETHODIMP Toggle() final { return Invoke(); }
    IFACEMETHODIMP get_ToggleState(::ToggleState*) final;

    // IValueProvider
    IFACEMETHODIMP SetValue(LPCWSTR) final;
    IFACEMETHODIMP get_Value(BSTR*) final;
    // IValueProvider and IRangeValueProvider
    IFACEMETHODIMP get_IsReadOnly(BOOL*) final;

    // IRangeValueProvider
    IFACEMETHODIMP SetValue(double) final;
    IFACEMETHODIMP get_Value(double*) final;
    IFACEMETHODIMP get_Maximum(double*) final;
    IFACEMETHODIMP get_Minimum(double*) final;
    IFACEMETHODIMP get_LargeChange(double*) final;
    IFACEMETHODIMP get_SmallChange(double*) final;

    // ITextProvider
    IFACEMETHODIMP GetSelection(SAFEARRAY**) final;
    IFACEMETHODIMP GetVisibleRanges(SAFEARRAY**) final;
    IFACEMETHODIMP RangeFromChild(IRawElementProviderSimple*, ITextRangeProvider**) final;
    IFACEMETHODIMP RangeFromPoint(UiaPoint, ITextRangeProvider**) final;
    IFACEMETHODIMP get_DocumentRange(ITextRangeProvider**) final;
    IFACEMETHODIMP get_SupportedTextSelection(SupportedTextSelection*) final;

private:
    ~Provider() = default;

    std::optional<RangeValue> range() const
    {
        auto* target = object();
        return target ? rangeValue(*target) : std::nullopt;
    }

    ULONG m_refCount { 1 };
    UIABridge* m_bridge;
    std::optional<AXID> m_id;
};

class UIABridge final : public Bridge {
public:
    UIABridge(View& view, HWND window)
        : Bridge(view)
        , m_window(window)
    {
        // The engine thread is the window's: an apartment of its own, where UI
        // Automation calls the providers (SDL may have made it one already).
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_uninitializeCOM = SUCCEEDED(result);
        SetPropW(m_window, s_property, this);
        m_originalProcedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(m_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProcedure)));
    }

    ~UIABridge()
    {
        SetWindowLongPtrW(m_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_originalProcedure));
        RemovePropW(m_window, s_property);
        // UI Automation forgets the window's root, then every element.
        UiaReturnRawElementProvider(m_window, 0, 0, nullptr);
        for (auto& provider : m_providers.values()) {
            provider->disconnect();
            UiaDisconnectProvider(provider.Get());
        }
        if (m_root) {
            m_root->disconnect();
            UiaDisconnectProvider(m_root.Get());
        }
        if (m_uninitializeCOM)
            CoUninitialize();
    }

    HWND window() const { return m_window; }

    Provider& root()
    {
        if (!m_root)
            m_root.Attach(new Provider(*this, std::nullopt));
        return *m_root.Get();
    }

    // The provider of an object, the same one each time.
    Provider& provider(Object& object)
    {
        auto id = object.objectID();
        auto result = m_providers.ensure(id, [&] {
            ComPtr<Provider> provider;
            provider.Attach(new Provider(*this, id));
            return provider;
        });
        return *result.iterator->value.Get();
    }

    // Logical pixels of the view to screen pixels, and back.
    UiaRect screenRect(const FloatRect& rect) const
    {
        POINT origin { 0, 0 };
        ClientToScreen(m_window, &origin);
        float scale = view().scale();
        return { origin.x + rect.x() * scale, origin.y + rect.y() * scale, rect.width() * scale, rect.height() * scale };
    }

    IntPoint viewPoint(double x, double y) const
    {
        POINT point { static_cast<LONG>(x), static_cast<LONG>(y) };
        ScreenToClient(m_window, &point);
        float scale = view().scale();
        return { static_cast<int>(point.x / scale), static_cast<int>(point.y / scale) };
    }

    void event(Object& object, Event event) final
    {
        if (!UiaClientsAreListening())
            return;
        auto& target = provider(object);
        switch (event) {
        case Event::FocusChanged:
            UiaRaiseAutomationEvent(&target, UIA_AutomationFocusChangedEventId);
            break;
        case Event::ValueChanged: {
            VARIANT oldValue { };
            VARIANT newValue { };
            if (auto range = rangeValue(object)) {
                newValue.vt = VT_R8;
                newValue.dblVal = range->current;
                UiaRaiseAutomationPropertyChangedEvent(&target, UIA_RangeValueValuePropertyId, oldValue, newValue);
            } else if (hasValuePattern(object)) {
                newValue.vt = VT_BSTR;
                newValue.bstrVal = toBSTR(value(object));
                UiaRaiseAutomationPropertyChangedEvent(&target, UIA_ValueValuePropertyId, oldValue, newValue);
                VariantClear(&newValue);
            }
            if (textSelection(object))
                UiaRaiseAutomationEvent(&target, UIA_Text_TextChangedEventId);
            break;
        }
        case Event::NameChanged: {
            VARIANT oldValue { };
            VARIANT newValue { };
            newValue.vt = VT_BSTR;
            newValue.bstrVal = toBSTR(name(object));
            UiaRaiseAutomationPropertyChangedEvent(&target, UIA_NamePropertyId, oldValue, newValue);
            VariantClear(&newValue);
            break;
        }
        case Event::StateChanged: {
            if (!isToggle(object))
                break;
            VARIANT oldValue { };
            VARIANT newValue { };
            newValue.vt = VT_I4;
            auto objectStates = states(object);
            newValue.lVal = objectStates.contains(State::Mixed) ? ToggleState_Indeterminate : objectStates.contains(State::Checked) || objectStates.contains(State::Pressed) ? ToggleState_On : ToggleState_Off;
            UiaRaiseAutomationPropertyChangedEvent(&target, UIA_ToggleToggleStatePropertyId, oldValue, newValue);
            break;
        }
        case Event::ChildrenChanged: {
            SAFEARRAY* runtimeId = nullptr;
            if (SUCCEEDED(target.GetRuntimeId(&runtimeId)) && runtimeId) {
                LONG count = 0;
                SafeArrayGetUBound(runtimeId, 1, &count);
                int* ids = nullptr;
                SafeArrayAccessData(runtimeId, reinterpret_cast<void**>(&ids));
                UiaRaiseStructureChangedEvent(&target, StructureChangeType_ChildrenInvalidated, ids, count + 1);
                SafeArrayUnaccessData(runtimeId);
                SafeArrayDestroy(runtimeId);
            }
            break;
        }
        case Event::LoadComplete:
            UiaRaiseAsyncContentLoadedEvent(&target, AsyncContentLoadedState_Completed, 100);
            break;
        }
    }

    void objectDetached(AXID id) final
    {
        auto provider = m_providers.take(id);
        if (!provider)
            return;
        provider->disconnect();
        UiaDisconnectProvider(provider.Get());
    }

private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* bridge = static_cast<UIABridge*>(GetPropW(window, s_property));
        if (!bridge)
            return DefWindowProcW(window, message, wParam, lParam);
        // UI Automation asks a window for its root element.
        if (message == WM_GETOBJECT && static_cast<long>(lParam) == static_cast<long>(UiaRootObjectId))
            return UiaReturnRawElementProvider(window, wParam, lParam, &bridge->root());
        return CallWindowProcW(bridge->m_originalProcedure, window, message, wParam, lParam);
    }

    static constexpr const wchar_t* s_property = L"ZeroGAccessibility";

    HWND m_window;
    WNDPROC m_originalProcedure { nullptr };
    bool m_uninitializeCOM { false };
    ComPtr<Provider> m_root;
    HashMap<AXID, ComPtr<Provider>> m_providers;
};

Object* Provider::object() const
{
    if (!m_bridge || !m_id || !isMainThread())
        return nullptr;
    return Accessibility::object(m_bridge->view(), *m_id);
}

#define RETURN_IF_DEFUNCT(target) \
    if (!(target)) \
        return UIA_E_ELEMENTNOTAVAILABLE

IFACEMETHODIMP Provider::GetPatternProvider(PATTERNID pattern, IUnknown** result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    if (isRoot())
        return m_bridge ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    switch (pattern) {
    case UIA_InvokePatternId:
        if (!isToggle(*target) && !defaultActionName(*target).isEmpty())
            *result = static_cast<IInvokeProvider*>(this);
        break;
    case UIA_TogglePatternId:
        if (isToggle(*target))
            *result = static_cast<IToggleProvider*>(this);
        break;
    case UIA_ValuePatternId:
        if (hasValuePattern(*target))
            *result = static_cast<IValueProvider*>(this);
        break;
    case UIA_RangeValuePatternId:
        if (rangeValue(*target))
            *result = static_cast<IRangeValueProvider*>(this);
        break;
    case UIA_TextPatternId:
        if (textSelection(*target))
            *result = static_cast<ITextProvider*>(this);
        break;
    default:
        break;
    }
    if (*result)
        AddRef();
    return S_OK;
}

IFACEMETHODIMP Provider::GetPropertyValue(PROPERTYID property, VARIANT* result)
{
    if (!result)
        return E_POINTER;
    VariantInit(result);
    if (isRoot()) {
        if (!m_bridge)
            return UIA_E_ELEMENTNOTAVAILABLE;
        // The rest comes from the HWND's host provider.
        if (property == UIA_FrameworkIdPropertyId) {
            result->vt = VT_BSTR;
            result->bstrVal = SysAllocString(L"ZeroG");
        }
        return S_OK;
    }
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    auto setBool = [&](bool value) {
        result->vt = VT_BOOL;
        result->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    };
    auto setString = [&](const String& value) {
        result->vt = VT_BSTR;
        result->bstrVal = toBSTR(value);
    };
    switch (property) {
    case UIA_NamePropertyId:
        setString(name(*target));
        break;
    case UIA_HelpTextPropertyId:
        setString(description(*target));
        break;
    case UIA_ControlTypePropertyId:
        result->vt = VT_I4;
        result->lVal = controlType(role(*target));
        break;
    case UIA_FrameworkIdPropertyId:
        result->vt = VT_BSTR;
        result->bstrVal = SysAllocString(L"ZeroG");
        break;
    case UIA_IsEnabledPropertyId:
        setBool(states(*target).contains(State::Enabled));
        break;
    case UIA_HasKeyboardFocusPropertyId:
        setBool(states(*target).contains(State::Focused));
        break;
    case UIA_IsKeyboardFocusablePropertyId:
        setBool(states(*target).contains(State::Focusable));
        break;
    case UIA_IsOffscreenPropertyId:
        setBool(states(*target).contains(State::Offscreen));
        break;
    case UIA_IsPasswordPropertyId:
        setBool(states(*target).contains(State::Password));
        break;
    case UIA_IsRequiredForFormPropertyId:
        setBool(states(*target).contains(State::Required));
        break;
    case UIA_IsControlElementPropertyId:
    case UIA_IsContentElementPropertyId:
        setBool(true);
        break;
    default:
        break;
    }
    return S_OK;
}

IFACEMETHODIMP Provider::get_HostRawElementProvider(IRawElementProviderSimple** result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    // The window's element is the HWND's: UI Automation adds its properties.
    if (isRoot() && m_bridge)
        return UiaHostProviderFromHwnd(m_bridge->window(), result);
    return S_OK;
}

IFACEMETHODIMP Provider::Navigate(NavigateDirection direction, IRawElementProviderFragment** result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    if (!m_bridge)
        return UIA_E_ELEMENTNOTAVAILABLE;
    auto& view = m_bridge->view();
    IRawElementProviderFragment* found = nullptr;
    if (isRoot()) {
        // The window's one child is the document.
        if (direction == NavigateDirection_FirstChild || direction == NavigateDirection_LastChild) {
            if (auto* document = Accessibility::root(view))
                found = &m_bridge->provider(*document);
        }
    } else {
        auto* target = object();
        RETURN_IF_DEFUNCT(target);
        switch (direction) {
        case NavigateDirection_Parent:
            if (auto* parentObject = parent(*target, view))
                found = &m_bridge->provider(*parentObject);
            else
                found = &m_bridge->root();
            break;
        case NavigateDirection_FirstChild:
        case NavigateDirection_LastChild: {
            auto objectChildren = children(*target);
            if (!objectChildren.isEmpty())
                found = &m_bridge->provider(direction == NavigateDirection_FirstChild ? objectChildren.first().get() : objectChildren.last().get());
            break;
        }
        case NavigateDirection_NextSibling:
        case NavigateDirection_PreviousSibling: {
            auto* parentObject = parent(*target, view);
            if (!parentObject)
                break; // the document has no siblings
            auto siblings = children(*parentObject);
            auto index = siblings.findIf([&](auto& sibling) { return sibling.ptr() == target; });
            if (index == notFound)
                break;
            if (direction == NavigateDirection_NextSibling && index + 1 < siblings.size())
                found = &m_bridge->provider(siblings[index + 1].get());
            else if (direction == NavigateDirection_PreviousSibling && index > 0)
                found = &m_bridge->provider(siblings[index - 1].get());
            break;
        }
        }
    }
    if (found) {
        found->AddRef();
        *result = found;
    }
    return S_OK;
}

IFACEMETHODIMP Provider::GetRuntimeId(SAFEARRAY** result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    if (isRoot())
        return m_bridge ? S_OK : UIA_E_ELEMENTNOTAVAILABLE; // the HWND's
    if (!object())
        return UIA_E_ELEMENTNOTAVAILABLE;
    // Unique within the window: the object's ID, in two halves.
    uint64_t id = m_id->toUInt64();
    int parts[] = { UiaAppendRuntimeId, static_cast<int>(id & 0xffffffff), static_cast<int>(id >> 32) };
    auto* array = SafeArrayCreateVector(VT_I4, 0, std::size(parts));
    if (!array)
        return E_OUTOFMEMORY;
    for (LONG i = 0; i < static_cast<LONG>(std::size(parts)); ++i)
        SafeArrayPutElement(array, &i, &parts[i]);
    *result = array;
    return S_OK;
}

IFACEMETHODIMP Provider::get_BoundingRectangle(UiaRect* result)
{
    if (!result)
        return E_POINTER;
    *result = { };
    if (isRoot())
        return m_bridge ? S_OK : UIA_E_ELEMENTNOTAVAILABLE; // the HWND's
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    *result = m_bridge->screenRect(bounds(*target));
    return S_OK;
}

IFACEMETHODIMP Provider::SetFocus()
{
    if (isRoot())
        return m_bridge ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    return setFocus(*target) ? S_OK : UIA_E_NOTSUPPORTED;
}

IFACEMETHODIMP Provider::get_FragmentRoot(IRawElementProviderFragmentRoot** result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    if (!m_bridge)
        return UIA_E_ELEMENTNOTAVAILABLE;
    auto& root = m_bridge->root();
    root.AddRef();
    *result = &root;
    return S_OK;
}

IFACEMETHODIMP Provider::ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    if (!m_bridge || !isMainThread())
        return UIA_E_ELEMENTNOTAVAILABLE;
    if (auto* hit = hitTest(m_bridge->view(), m_bridge->viewPoint(x, y))) {
        auto& provider = m_bridge->provider(*hit);
        provider.AddRef();
        *result = &provider;
    }
    return S_OK;
}

IFACEMETHODIMP Provider::GetFocus(IRawElementProviderFragment** result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    if (!m_bridge || !isMainThread())
        return UIA_E_ELEMENTNOTAVAILABLE;
    if (auto* focusedObject = focused(m_bridge->view())) {
        auto& provider = m_bridge->provider(*focusedObject);
        provider.AddRef();
        *result = &provider;
    }
    return S_OK;
}

IFACEMETHODIMP Provider::Invoke()
{
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    if (!states(*target).contains(State::Enabled))
        return UIA_E_ELEMENTNOTENABLED;
    return doDefaultAction(*target) ? S_OK : UIA_E_NOTSUPPORTED;
}

IFACEMETHODIMP Provider::get_ToggleState(::ToggleState* result)
{
    if (!result)
        return E_POINTER;
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    auto objectStates = states(*target);
    *result = objectStates.contains(State::Mixed) ? ToggleState_Indeterminate : objectStates.contains(State::Checked) || objectStates.contains(State::Pressed) ? ToggleState_On : ToggleState_Off;
    return S_OK;
}

IFACEMETHODIMP Provider::SetValue(LPCWSTR text)
{
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    if (!canSetValue(*target))
        return UIA_E_ELEMENTNOTENABLED;
    return setValue(*target, fromWide(text)) ? S_OK : UIA_E_ELEMENTNOTENABLED;
}

IFACEMETHODIMP Provider::get_Value(BSTR* result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    *result = toBSTR(value(*target));
    return S_OK;
}

IFACEMETHODIMP Provider::get_IsReadOnly(BOOL* result)
{
    if (!result)
        return E_POINTER;
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    *result = !canSetValue(*target);
    return S_OK;
}

IFACEMETHODIMP Provider::SetValue(double number)
{
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    return setRangeValue(*target, number) ? S_OK : UIA_E_ELEMENTNOTENABLED;
}

IFACEMETHODIMP Provider::get_Value(double* result)
{
    if (!result)
        return E_POINTER;
    auto current = range();
    RETURN_IF_DEFUNCT(current);
    *result = current->current;
    return S_OK;
}

IFACEMETHODIMP Provider::get_Maximum(double* result)
{
    if (!result)
        return E_POINTER;
    auto current = range();
    RETURN_IF_DEFUNCT(current);
    *result = current->maximum;
    return S_OK;
}

IFACEMETHODIMP Provider::get_Minimum(double* result)
{
    if (!result)
        return E_POINTER;
    auto current = range();
    RETURN_IF_DEFUNCT(current);
    *result = current->minimum;
    return S_OK;
}

IFACEMETHODIMP Provider::get_LargeChange(double* result)
{
    if (!result)
        return E_POINTER;
    auto current = range();
    RETURN_IF_DEFUNCT(current);
    *result = (current->maximum - current->minimum) / 10;
    return S_OK;
}

IFACEMETHODIMP Provider::get_SmallChange(double* result)
{
    if (!result)
        return E_POINTER;
    auto current = range();
    RETURN_IF_DEFUNCT(current);
    *result = (current->maximum - current->minimum) / 100;
    return S_OK;
}

// ---- Text ----

// Where text units end: the offset `count` units from `offset` (count may be
// negative), and how many units it moved.
unsigned unitBoundary(const String& text, TextUnit unit, unsigned offset, int count, int& moved)
{
    unsigned length = text.length();
    auto isSpace = [&](unsigned index) {
        return text[index] == ' ' || text[index] == '\t' || text[index] == '\n' || text[index] == '\r';
    };
    // A unit starts there: any character; a word after spaces; a line after a
    // line break. Formats, pages and the document are the whole text.
    auto startsUnit = [&](unsigned index) {
        if (!index || index >= length)
            return true;
        switch (unit) {
        case TextUnit_Character:
            return true;
        case TextUnit_Word:
            return !isSpace(index) && isSpace(index - 1);
        case TextUnit_Line:
        case TextUnit_Paragraph:
            return text[index - 1] == '\n';
        default:
            return false;
        }
    };
    moved = 0;
    while (count > 0 && offset < length) {
        do
            ++offset;
        while (offset < length && !startsUnit(offset));
        --count;
        ++moved;
    }
    while (count < 0 && offset > 0) {
        do
            --offset;
        while (offset > 0 && !startsUnit(offset));
        ++count;
        --moved;
    }
    return offset;
}

// The start of the unit containing `offset`.
unsigned unitStart(const String& text, TextUnit unit, unsigned offset)
{
    int moved;
    unsigned length = text.length();
    if (offset >= length)
        return length;
    // One unit forward then back lands on the start of the unit at `offset`.
    unsigned next = unitBoundary(text, unit, offset, 1, moved);
    return unitBoundary(text, unit, next, -1, moved);
}

// A range of an element's text (UTF-16 offsets, read again on each call, so a
// range of a field being edited stays within its text).
class TextRange final : public ITextRangeProvider {
public:
    TextRange(Provider& owner, unsigned start, unsigned end)
        : m_owner(&owner)
        , m_start(start)
        , m_end(end)
    {
    }

    static ITextRangeProvider* create(Provider& owner, unsigned start, unsigned end)
    {
        return new TextRange(owner, start, end);
    }

    IFACEMETHODIMP QueryInterface(REFIID iid, void** result) final
    {
        if (!result)
            return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ITextRangeProvider)) {
            *result = static_cast<ITextRangeProvider*>(this);
            AddRef();
            return S_OK;
        }
        *result = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() final { return ++m_refCount; }
    IFACEMETHODIMP_(ULONG) Release() final
    {
        auto count = --m_refCount;
        if (!count)
            delete this;
        return count;
    }

    IFACEMETHODIMP Clone(ITextRangeProvider** result) final
    {
        if (!result)
            return E_POINTER;
        *result = create(*m_owner.Get(), m_start, m_end);
        return S_OK;
    }

    IFACEMETHODIMP Compare(ITextRangeProvider* other, BOOL* result) final
    {
        if (!result)
            return E_POINTER;
        auto* range = cast(other);
        *result = range && range->m_owner.Get() == m_owner.Get() && range->m_start == m_start && range->m_end == m_end;
        return S_OK;
    }

    IFACEMETHODIMP CompareEndpoints(TextPatternRangeEndpoint endpoint, ITextRangeProvider* other, TextPatternRangeEndpoint otherEndpoint, int* result) final
    {
        auto* range = cast(other);
        if (!result || !range)
            return E_INVALIDARG;
        *result = static_cast<int>(this->endpoint(endpoint)) - static_cast<int>(range->endpoint(otherEndpoint));
        return S_OK;
    }

    IFACEMETHODIMP ExpandToEnclosingUnit(TextUnit unit) final
    {
        auto contents = text();
        if (!contents)
            return UIA_E_ELEMENTNOTAVAILABLE;
        clamp(*contents);
        if (unit >= TextUnit_Page) {
            m_start = 0;
            m_end = contents->length();
            return S_OK;
        }
        int moved;
        m_start = unitStart(*contents, unit, m_start);
        m_end = unitBoundary(*contents, unit, m_start, 1, moved);
        return S_OK;
    }

    IFACEMETHODIMP FindAttribute(TEXTATTRIBUTEID, VARIANT, BOOL, ITextRangeProvider** result) final
    {
        if (!result)
            return E_POINTER;
        *result = nullptr; // no text attributes
        return S_OK;
    }

    IFACEMETHODIMP FindText(BSTR search, BOOL backward, BOOL ignoreCase, ITextRangeProvider** result) final
    {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        auto contents = text();
        if (!contents)
            return UIA_E_ELEMENTNOTAVAILABLE;
        clamp(*contents);
        auto needle = fromWide(search);
        auto haystack = contents->substring(m_start, m_end - m_start);
        if (ignoreCase) {
            needle = needle.convertToASCIILowercase();
            haystack = haystack.convertToASCIILowercase();
        }
        size_t found = backward ? haystack.reverseFind(needle) : haystack.find(needle);
        if (found != notFound && !needle.isEmpty())
            *result = create(*m_owner.Get(), m_start + found, m_start + found + needle.length());
        return S_OK;
    }

    IFACEMETHODIMP GetAttributeValue(TEXTATTRIBUTEID, VARIANT* result) final
    {
        if (!result)
            return E_POINTER;
        VariantInit(result);
        IUnknown* notSupported = nullptr;
        UiaGetReservedNotSupportedValue(&notSupported);
        result->vt = VT_UNKNOWN;
        result->punkVal = notSupported;
        return S_OK;
    }

    IFACEMETHODIMP GetBoundingRectangles(SAFEARRAY** result) final
    {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        auto* target = m_owner->object();
        if (!target)
            return UIA_E_ELEMENTNOTAVAILABLE;
        // The element's box: no per-character geometry in this basic pattern.
        auto rect = m_owner->bridge()->screenRect(bounds(*target));
        double values[] = { rect.left, rect.top, rect.width, rect.height };
        auto* array = SafeArrayCreateVector(VT_R8, 0, std::size(values));
        if (!array)
            return E_OUTOFMEMORY;
        for (LONG i = 0; i < static_cast<LONG>(std::size(values)); ++i)
            SafeArrayPutElement(array, &i, &values[i]);
        *result = array;
        return S_OK;
    }

    IFACEMETHODIMP GetEnclosingElement(IRawElementProviderSimple** result) final
    {
        if (!result)
            return E_POINTER;
        auto* owner = static_cast<IRawElementProviderSimple*>(m_owner.Get());
        owner->AddRef();
        *result = owner;
        return S_OK;
    }

    IFACEMETHODIMP GetText(int maxLength, BSTR* result) final
    {
        if (!result)
            return E_POINTER;
        auto contents = text();
        if (!contents)
            return UIA_E_ELEMENTNOTAVAILABLE;
        clamp(*contents);
        unsigned length = m_end - m_start;
        if (maxLength >= 0)
            length = std::min<unsigned>(length, maxLength);
        *result = toBSTR(contents->substring(m_start, length));
        return S_OK;
    }

    IFACEMETHODIMP Move(TextUnit unit, int count, int* result) final
    {
        if (!result)
            return E_POINTER;
        auto contents = text();
        if (!contents)
            return UIA_E_ELEMENTNOTAVAILABLE;
        clamp(*contents);
        bool degenerate = m_start == m_end;
        int moved;
        // From the start of the unit, `count` units on; a range covers one unit
        // again, a degenerate range (a caret) stays degenerate.
        unsigned start = unitStart(*contents, unit >= TextUnit_Page ? TextUnit_Character : unit, m_start);
        m_start = unitBoundary(*contents, unit, start, count, moved);
        m_end = m_start;
        if (!degenerate) {
            int unitLength;
            m_end = unitBoundary(*contents, unit, m_start, 1, unitLength);
        }
        *result = moved;
        return S_OK;
    }

    IFACEMETHODIMP MoveEndpointByUnit(TextPatternRangeEndpoint endpoint, TextUnit unit, int count, int* result) final
    {
        if (!result)
            return E_POINTER;
        auto contents = text();
        if (!contents)
            return UIA_E_ELEMENTNOTAVAILABLE;
        clamp(*contents);
        unsigned moved = unitBoundary(*contents, unit, this->endpoint(endpoint), count, *result);
        setEndpoint(endpoint, moved);
        return S_OK;
    }

    IFACEMETHODIMP MoveEndpointByRange(TextPatternRangeEndpoint endpoint, ITextRangeProvider* other, TextPatternRangeEndpoint otherEndpoint) final
    {
        auto* range = cast(other);
        if (!range)
            return E_INVALIDARG;
        setEndpoint(endpoint, range->endpoint(otherEndpoint));
        return S_OK;
    }

    IFACEMETHODIMP Select() final
    {
        auto* target = m_owner->object();
        if (!target)
            return UIA_E_ELEMENTNOTAVAILABLE;
        return setTextSelection(*target, { m_start, m_end }) ? S_OK : UIA_E_INVALIDOPERATION;
    }

    IFACEMETHODIMP AddToSelection() final { return UIA_E_INVALIDOPERATION; }
    IFACEMETHODIMP RemoveFromSelection() final { return UIA_E_INVALIDOPERATION; }
    IFACEMETHODIMP ScrollIntoView(BOOL) final { return S_OK; }

    IFACEMETHODIMP GetChildren(SAFEARRAY** result) final
    {
        if (!result)
            return E_POINTER;
        *result = SafeArrayCreateVector(VT_UNKNOWN, 0, 0);
        return *result ? S_OK : E_OUTOFMEMORY;
    }

private:
    ~TextRange() = default;

    // Ranges given back by UI Automation are this class's, from this module.
    static TextRange* cast(ITextRangeProvider* range) { return static_cast<TextRange*>(range); }

    std::optional<String> text() const
    {
        auto* target = m_owner->object();
        if (!target)
            return std::nullopt;
        return Accessibility::text(*target);
    }

    void clamp(const String& contents)
    {
        m_end = std::min<unsigned>(m_end, contents.length());
        m_start = std::min(m_start, m_end);
    }

    unsigned endpoint(TextPatternRangeEndpoint endpoint) const
    {
        return endpoint == TextPatternRangeEndpoint_Start ? m_start : m_end;
    }

    void setEndpoint(TextPatternRangeEndpoint endpoint, unsigned offset)
    {
        if (endpoint == TextPatternRangeEndpoint_Start) {
            m_start = offset;
            m_end = std::max(m_end, offset);
        } else {
            m_end = offset;
            m_start = std::min(m_start, offset);
        }
    }

    std::atomic<ULONG> m_refCount { 1 };
    ComPtr<Provider> m_owner;
    unsigned m_start;
    unsigned m_end;
};

SAFEARRAY* rangeArray(Provider& owner, std::optional<TextSelection> range)
{
    auto* array = SafeArrayCreateVector(VT_UNKNOWN, 0, range ? 1 : 0);
    if (!array || !range)
        return array;
    auto* provider = TextRange::create(owner, range->start, range->end);
    LONG index = 0;
    SafeArrayPutElement(array, &index, provider); // the array holds its own reference
    provider->Release();
    return array;
}

IFACEMETHODIMP Provider::GetSelection(SAFEARRAY** result)
{
    if (!result)
        return E_POINTER;
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    *result = rangeArray(*this, textSelection(*target));
    return *result ? S_OK : E_OUTOFMEMORY;
}

IFACEMETHODIMP Provider::GetVisibleRanges(SAFEARRAY** result)
{
    if (!result)
        return E_POINTER;
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    *result = rangeArray(*this, TextSelection { 0, Accessibility::text(*target).length() });
    return *result ? S_OK : E_OUTOFMEMORY;
}

IFACEMETHODIMP Provider::RangeFromChild(IRawElementProviderSimple*, ITextRangeProvider** result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    return E_INVALIDARG; // a text field has no children with text of their own
}

IFACEMETHODIMP Provider::RangeFromPoint(UiaPoint, ITextRangeProvider** result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    RETURN_IF_DEFUNCT(object());
    // No per-character geometry: the start of the text.
    *result = TextRange::create(*this, 0, 0);
    return S_OK;
}

IFACEMETHODIMP Provider::get_DocumentRange(ITextRangeProvider** result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    auto* target = object();
    RETURN_IF_DEFUNCT(target);
    *result = TextRange::create(*this, 0, Accessibility::text(*target).length());
    return S_OK;
}

IFACEMETHODIMP Provider::get_SupportedTextSelection(SupportedTextSelection* result)
{
    if (!result)
        return E_POINTER;
    *result = SupportedTextSelection_Single;
    return S_OK;
}

#undef RETURN_IF_DEFUNCT

} // namespace

std::unique_ptr<Bridge> createBridge(View& view, void* nativeWindow, const String&)
{
    if (!nativeWindow)
        return nullptr;
    return std::make_unique<UIABridge>(view, static_cast<HWND>(nativeWindow));
}

} // namespace ZeroG::Accessibility

#endif // OS(WINDOWS)
