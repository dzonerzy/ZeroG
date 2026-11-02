// Notifications on Windows: toasts (Windows.UI.Notifications over WRL, the
// SDK's WinRT ABI headers; runtimeobject.lib). An application without a
// package or Start menu shortcut gets an identity by its AppUserModelID,
// registered for the user with the name and icon the notification center
// shows (HKCU\Software\Classes\AppUserModelId\<id>), and set as the
// process's. A click on a toast, or one of its buttons, comes as its
// Activated event on a thread pool thread: posted to the engine thread.

#include "config.h"
#include "desktop/Desktop.h"

#include <roapi.h>
#include <shobjidl_core.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>
#include <wtf/HashMap.h>
#include <wtf/HexNumber.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringToIntegerConversion.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
namespace Notification = ABI::Windows::UI::Notifications;
namespace Xml = ABI::Windows::Data::Xml::Dom;

namespace ZeroG::Notifications {

namespace {

constexpr const wchar_t* s_group = L"zerog";

struct State {
    Config config;
    Vector<wchar_t> appId; // NUL-terminated
    ComPtr<Notification::IToastNotificationManagerStatics> manager;
    ComPtr<Notification::IToastNotifier> notifier;
    // Kept while shown: their Activated handlers.
    HashMap<String, ComPtr<Notification::IToastNotification>> shown;
    unsigned untagged { 0 };
};

State& state()
{
    static NeverDestroyed<State> instance;
    return instance;
}

// NUL-terminated UTF-16 (wideCharacters terminates all but empty strings).
Vector<wchar_t> wide(const String& text)
{
    auto result = text.wideCharacters();
    if (result.isEmpty() || result.last())
        result.append(0);
    return result;
}

// A NUL-terminated buffer as an HSTRING of its own (an HStringReference
// cannot be returned: its handle points into the object).
HString reference(const Vector<wchar_t>& buffer)
{
    HString result;
    result.Set(buffer.span().data(), static_cast<unsigned>(buffer.size() - 1));
    return result;
}

String escape(const String& text)
{
    return makeStringByReplacingAll(makeStringByReplacingAll(makeStringByReplacingAll(makeStringByReplacingAll(text, '&', "&amp;"_s), '<', "&lt;"_s), '>', "&gt;"_s), '"', "&quot;"_s);
}

void registerAppId(const Config& config)
{
    auto key = wide(makeString("Software\\Classes\\AppUserModelId\\"_s, config.appId));
    HKEY handle;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.span().data(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS)
        return;
    auto setString = [&](const wchar_t* name, const String& value) {
        auto data = wide(value);
        RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.span().data()), static_cast<DWORD>(data.size() * sizeof(wchar_t)));
    };
    setString(L"DisplayName", config.appName.isEmpty() ? config.appId : config.appName);
    if (!config.iconPath.isEmpty())
        setString(L"IconUri", config.iconPath);
    RegCloseKey(handle);
}

} // namespace

Result configure(Config&& config, String& error)
{
    // The engine thread's apartment, whichever COM gave it before.
    HRESULT result = RoInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
        error = "WinRT is not available"_s;
        return Result::Unsupported;
    }
    auto& s = state();
    registerAppId(config);
    s.appId = wide(config.appId);
    SetCurrentProcessExplicitAppUserModelID(s.appId.span().data());
    ComPtr<Notification::IToastNotificationManagerStatics> manager;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(), IID_PPV_ARGS(&manager)))) {
        error = "the notification center is not available"_s;
        return Result::Unsupported;
    }
    ComPtr<Notification::IToastNotifier> notifier;
    if (FAILED(manager->CreateToastNotifierWithId(reference(s.appId).Get(), &notifier))) {
        error = "no notifier for this application id"_s;
        return Result::Failed;
    }
    s.manager = WTF::move(manager);
    s.notifier = WTF::move(notifier);
    s.config = WTF::move(config);
    return Result::Ok;
}

Result show(const String& title, const String& body, const String& tag, const Vector<String>& actions, String& error)
{
    auto& s = state();
    if (!s.notifier) {
        error = "the application did not say who it is (zg_runtime_set_notifications)"_s;
        return Result::Failed;
    }
    StringBuilder xml;
    xml.append("<toast launch=\"action=-1\"><visual><binding template=\"ToastGeneric\"><text>"_s, escape(title), "</text>"_s);
    if (!body.isEmpty())
        xml.append("<text>"_s, escape(body), "</text>"_s);
    xml.append("</binding></visual>"_s);
    if (!actions.isEmpty()) {
        xml.append("<actions>"_s);
        for (size_t i = 0; i < actions.size(); ++i)
            xml.append("<action content=\""_s, escape(actions[i]), "\" arguments=\"action="_s, String::number(i), "\" activationType=\"foreground\"/>"_s);
        xml.append("</actions>"_s);
    }
    xml.append("</toast>"_s);

    ComPtr<IInspectable> instance;
    ComPtr<Xml::IXmlDocument> document;
    ComPtr<Xml::IXmlDocumentIO> documentIO;
    auto text = wide(xml.toString());
    HRESULT made = RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), &instance);
    if (SUCCEEDED(made))
        made = instance.As(&document);
    if (SUCCEEDED(made))
        made = document.As(&documentIO);
    if (SUCCEEDED(made))
        made = documentIO->LoadXml(reference(text).Get());
    if (FAILED(made)) {
        error = makeString("the notification's text could not be laid out (0x"_s, hex(static_cast<uint32_t>(made)), ')');
        return Result::Failed;
    }
    ComPtr<Notification::IToastNotificationFactory> factory;
    ComPtr<Notification::IToastNotification> toast;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(), IID_PPV_ARGS(&factory)))
        || FAILED(factory->CreateToastNotification(document.Get(), &toast))) {
        error = "the notification could not be made"_s;
        return Result::Failed;
    }
    String key = tag.isEmpty() ? makeString("zerog-"_s, ++s.untagged) : tag;
    auto keyText = wide(key);
    ComPtr<Notification::IToastNotification2> tagged;
    if (SUCCEEDED(toast.As(&tagged))) {
        tagged->put_Tag(reference(keyText).Get());
        tagged->put_Group(HStringReference(s_group).Get());
    }
    using ActivatedHandler = ABI::Windows::Foundation::ITypedEventHandler<Notification::ToastNotification*, IInspectable*>;
    EventRegistrationToken token;
    toast->add_Activated(Callback<ActivatedHandler>([key = key.isolatedCopy()](Notification::IToastNotification*, IInspectable* arguments) -> HRESULT {
        int action = -1;
        ComPtr<Notification::IToastActivatedEventArgs> activated;
        HString launch;
        if (arguments && SUCCEEDED(arguments->QueryInterface(IID_PPV_ARGS(&activated))) && SUCCEEDED(activated->get_Arguments(launch.GetAddressOf()))) {
            UINT32 length = 0;
            auto* raw = launch.GetRawBuffer(&length);
            String text(std::span { reinterpret_cast<const char16_t*>(raw), length });
            if (text.startsWith("action="_s))
                action = parseInteger<int>(StringView(text).substring(7)).value_or(-1);
        }
        callOnMainThread([key = key.isolatedCopy(), action] {
            if (auto& activated = state().config.activated)
                activated(key, action);
        });
        return S_OK;
    }).Get(), &token);
    if (FAILED(s.notifier->Show(toast.Get()))) {
        error = "the notification center refused it (notifications off for this application?)"_s;
        return Result::Failed;
    }
    s.shown.set(key, WTF::move(toast));
    return Result::Ok;
}

Result remove(const String& tag, String& error)
{
    auto& s = state();
    if (!s.manager) {
        error = "the application did not say who it is (zg_runtime_set_notifications)"_s;
        return Result::Failed;
    }
    ComPtr<Notification::IToastNotificationManagerStatics2> manager;
    ComPtr<Notification::IToastNotificationHistory> history;
    if (FAILED(s.manager.As(&manager)) || FAILED(manager->get_History(&history))) {
        error = "the notification history is not available"_s;
        return Result::Unsupported;
    }
    auto tagText = wide(tag);
    history->RemoveGroupedTagWithId(reference(tagText).Get(), HStringReference(s_group).Get(), reference(s.appId).Get());
    s.shown.remove(tag);
    return Result::Ok;
}

} // namespace ZeroG::Notifications
