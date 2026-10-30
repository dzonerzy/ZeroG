// Accessibility on Windows (ZeroG.md, "Accessibilità"): a UI Automation client,
// on a thread of its own as a screen reader would be, finds the page's controls
// in the window by name and uses them: a button is invoked, a text field read and
// set, a check box toggled, a slider read; moving the focus to the text field
// raises a focus event for it.
//
// Usage: accessibility   (needs a desktop)

#include "config.h"
#include "Embedding.h"
#include "window/Window.h"

#include <UIAutomation.h>
#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>
#include <wrl/client.h>
#include <wtf/text/WTFString.h>

using Microsoft::WRL::ComPtr;
using namespace WebCore;

namespace {

constexpr auto page = R"HTML(<!DOCTYPE html>
<html><body>
  <button id="button" onclick="window.clicks = (window.clicks || 0) + 1">Press me</button>
  <input id="field" aria-label="Your name" value="abc">
  <input id="box" type="checkbox" aria-label="Agree">
  <input id="volume" type="range" min="0" max="10" value="3" aria-label="Volume">
</body></html>)HTML";

#define check(condition, ...) [&] {                    \
        bool result = (condition);                     \
        std::printf("%s ", result ? "ok  " : "FAIL");  \
        std::printf(__VA_ARGS__);                      \
        std::printf("\n");                             \
        return result;                                 \
    }()

bool runUntil(ZeroG::Window& window, Seconds timeout, const std::function<bool()>& condition)
{
    auto deadline = MonotonicTime::now() + timeout;
    while (!condition()) {
        if (MonotonicTime::now() > deadline || !window.runOnce(10_ms))
            return false;
    }
    return true;
}

std::string narrow(BSTR text)
{
    if (!text)
        return { };
    int length = WideCharToMultiByte(CP_UTF8, 0, text, SysStringLen(text), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, SysStringLen(text), result.data(), length, nullptr, nullptr);
    return result;
}

// Focus events, as a screen reader gets them (on a UI Automation thread).
class FocusHandler final : public IUIAutomationFocusChangedEventHandler {
public:
    IFACEMETHODIMP QueryInterface(REFIID iid, void** result) final
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IUIAutomationFocusChangedEventHandler)) {
            *result = static_cast<IUIAutomationFocusChangedEventHandler*>(this);
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
    IFACEMETHODIMP HandleFocusChangedEvent(IUIAutomationElement* element) final
    {
        BSTR name = nullptr;
        if (element && SUCCEEDED(element->get_CurrentName(&name)) && narrow(name) == "Your name")
            focusedField = true;
        SysFreeString(name);
        return S_OK;
    }

    std::atomic<bool> focusedField { false };

private:
    std::atomic<ULONG> m_refCount { 1 };
};

// What the client found, read by the main thread once it is done.
struct Results {
    bool connected { false };
    bool foundButton { false };
    bool isButton { false };
    bool invoked { false };
    std::string fieldValue;
    bool setField { false };
    bool toggled { false };
    double volume { -1 };
    double volumeMaximum { -1 };
    bool focusEvent { false };
    std::atomic<bool> done { false };
};

ComPtr<IUIAutomationElement> findByName(IUIAutomation& automation, IUIAutomationElement& root, const wchar_t* name)
{
    VARIANT value;
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(name);
    ComPtr<IUIAutomationCondition> condition;
    automation.CreatePropertyCondition(UIA_NamePropertyId, value, &condition);
    VariantClear(&value);
    ComPtr<IUIAutomationElement> element;
    if (condition)
        root.FindFirst(TreeScope_Descendants, condition.Get(), &element);
    return element;
}

void runClient(HWND window, Results& results)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IUIAutomation> automation;
        CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
        ComPtr<IUIAutomationElement> root;
        if (automation)
            automation->ElementFromHandle(window, &root);
        results.connected = !!root;
        if (root) {
            if (auto button = findByName(*automation.Get(), *root.Get(), L"Press me")) {
                results.foundButton = true;
                CONTROLTYPEID type = 0;
                button->get_CurrentControlType(&type);
                results.isButton = type == UIA_ButtonControlTypeId;
                ComPtr<IUIAutomationInvokePattern> invoke;
                button->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke));
                results.invoked = invoke && SUCCEEDED(invoke->Invoke());
            }
            if (auto field = findByName(*automation.Get(), *root.Get(), L"Your name")) {
                // The focus moves there: a screen reader hears of it.
                auto* handler = new FocusHandler;
                automation->AddFocusChangedEventHandler(nullptr, handler);
                field->SetFocus();
                for (int i = 0; i < 200 && !handler->focusedField; ++i)
                    Sleep(10);
                results.focusEvent = handler->focusedField;
                automation->RemoveFocusChangedEventHandler(handler);
                handler->Release();

                ComPtr<IUIAutomationValuePattern> valuePattern;
                field->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&valuePattern));
                if (valuePattern) {
                    BSTR value = nullptr;
                    valuePattern->get_CurrentValue(&value);
                    results.fieldValue = narrow(value);
                    SysFreeString(value);
                    BSTR newValue = SysAllocString(L"xyz");
                    results.setField = SUCCEEDED(valuePattern->SetValue(newValue));
                    SysFreeString(newValue);
                }
            }
            if (auto box = findByName(*automation.Get(), *root.Get(), L"Agree")) {
                ComPtr<IUIAutomationTogglePattern> toggle;
                box->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle));
                ::ToggleState state = ToggleState_Indeterminate;
                if (toggle && SUCCEEDED(toggle->Toggle()))
                    toggle->get_CurrentToggleState(&state);
                results.toggled = state == ToggleState_On;
            }
            if (auto volume = findByName(*automation.Get(), *root.Get(), L"Volume")) {
                ComPtr<IUIAutomationRangeValuePattern> range;
                volume->GetCurrentPatternAs(UIA_RangeValuePatternId, IID_PPV_ARGS(&range));
                if (range) {
                    range->get_CurrentValue(&results.volume);
                    range->get_CurrentMaximum(&results.volumeMaximum);
                }
            }
        }
    }
    CoUninitialize();
    results.done = true;
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;

    auto window = ZeroG::Window::create({ "ZeroG accessibility test"_s, { 400, 200 }, true, false, true });
    if (!check(!!window, "window with a GPU %s", ZeroG::Window::lastError().utf8().data()))
        return 1;
    window->view().loadHTML(String::fromUTF8(page));
    window->view().setFocused(true);
    runUntil(*window, 500_ms, [] { return false; });

    // The client works while this thread runs the window's loop, where UI
    // Automation's calls to the page are made.
    Results results;
    std::thread client(runClient, static_cast<HWND>(window->nativeHandle()), std::ref(results));
    bool finished = runUntil(*window, 60_s, [&] { return results.done.load(); });
    client.join();
    ok &= check(finished, "the client finished");

    ok &= check(results.connected, "UI Automation reaches the window's page");
    ok &= check(results.foundButton && results.isButton, "the button is found by its name, as a button");
    ok &= check(results.invoked, "Invoke on it");
    runUntil(*window, 1_s, [&] { return window->view().evaluateScript("String(window.clicks)"_s) == "1"_s; });
    auto clicks = window->view().evaluateScript("String(window.clicks)"_s);
    ok &= check(clicks == "1"_s, "the page saw the click: %s", clicks.utf8().data());
    ok &= check(results.fieldValue == "abc", "the text field's value: '%s'", results.fieldValue.c_str());
    auto field = window->view().evaluateScript("document.getElementById('field').value"_s);
    ok &= check(results.setField && field == "xyz"_s, "a value set through UI Automation reaches the page: '%s'", field.utf8().data());
    ok &= check(results.focusEvent, "focusing the field raises a focus event for it");
    auto checked = window->view().evaluateScript("String(document.getElementById('box').checked)"_s);
    ok &= check(results.toggled && checked == "true"_s, "the check box toggled on: %s", checked.utf8().data());
    ok &= check(results.volume == 3 && results.volumeMaximum == 10, "the slider's range value: %g of %g", results.volume, results.volumeMaximum);

    window = nullptr;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
