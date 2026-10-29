// Direct JavaScript bindings (ZeroG.md, "Binding"): native functions installed on
// the main frame's window object of trusted origins, called synchronously through
// the JSC C API (no IPC), with Promises settled later from any thread. Also the
// values of zg_view_evaluate and zg_view_emit.

#include "config.h"
#include "APIInternal.h"

#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/JSCJSValueInlines.h>
#include <JavaScriptCore/JSNativeStdFunction.h>
#include <JavaScriptCore/OpaqueJSString.h>
#include <JavaScriptCore/JSContextRef.h>
#include <JavaScriptCore/JSObjectRef.h>
#include <JavaScriptCore/JSStringRef.h>
#include <JavaScriptCore/JSValueRef.h>
#include <WebCore/DOMWrapperWorld.h>
#include <WebCore/Document.h>
#include <WebCore/JSDOMGlobalObject.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/Page.h>
#include <WebCore/ScriptController.h>
#include <atomic>
#include <cmath>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

using namespace WebCore;
using namespace ZeroG;
using namespace ZeroG::API;

struct zg_value {
    enum class Kind : uint8_t { Plain, JSON, Throw, Promise };
    Kind kind { Kind::Plain };
    zg_value_type type { ZG_TYPE_UNDEFINED };
    bool boolean { false };
    double number { 0 };
    int64_t integer { 0 };
    bool isInt64 { false };
    std::string text; // string, JSON, or the message to throw
    JSObjectRef promise { nullptr };
};

struct zg_call {
    JSContextRef context { nullptr };
    size_t argc { 0 };
    const JSValueRef* argv { nullptr };
    zg_view* view { nullptr };
    std::string origin;
    std::vector<std::unique_ptr<std::string>> strings; // arguments read as text
    std::vector<std::unique_ptr<zg_value>> values; // returned values, freed after the call
};

struct zg_promise {
    JSGlobalContextRef context { nullptr };
    JSObjectRef resolve { nullptr };
    JSObjectRef reject { nullptr };
    std::atomic<bool> settled { false };
};

namespace {

// The whole UTF-8 text, embedded NULs included (JSStringCreateWithUTF8CString
// stops at the first one); invalid sequences become U+FFFD.
struct JSStringHolder {
    explicit JSStringHolder(const std::string& text)
        : string(OpaqueJSString::tryCreate(String::fromUTF8ReplacingInvalidSequences(byteCast<char8_t>(std::span { text.data(), text.size() }))).leakRef())
    {
    }
    ~JSStringHolder() { JSStringRelease(string); }
    JSStringRef string;
};

std::string utf8(JSStringRef string)
{
    size_t maximum = JSStringGetMaximumUTF8CStringSize(string);
    std::string result(maximum, '\0');
    size_t written = JSStringGetUTF8CString(string, result.data(), maximum);
    result.resize(written ? written - 1 : 0);
    return result;
}

std::string toUTF8(JSContextRef context, JSValueRef value)
{
    JSStringRef string = JSValueToStringCopy(context, value, nullptr);
    if (!string)
        return { };
    auto result = utf8(string);
    JSStringRelease(string);
    return result;
}

std::string toJSON(JSContextRef context, JSValueRef value)
{
    JSStringRef json = JSValueCreateJSONString(context, value, 0, nullptr);
    if (!json)
        return "null"; // undefined, functions
    auto result = utf8(json);
    JSStringRelease(json);
    return result;
}

zg_value_type typeOf(JSContextRef context, JSValueRef value)
{
    switch (JSValueGetType(context, value)) {
    case kJSTypeUndefined:
        return ZG_TYPE_UNDEFINED;
    case kJSTypeNull:
        return ZG_TYPE_NULL;
    case kJSTypeBoolean:
        return ZG_TYPE_BOOLEAN;
    case kJSTypeNumber:
        return ZG_TYPE_NUMBER;
    case kJSTypeString:
        return ZG_TYPE_STRING;
    case kJSTypeBigInt:
        return ZG_TYPE_BIGINT;
    case kJSTypeObject: {
        if (JSValueIsArray(context, value))
            return ZG_TYPE_ARRAY;
        auto* object = JSValueToObject(context, value, nullptr);
        return object && JSObjectIsFunction(context, object) ? ZG_TYPE_FUNCTION : ZG_TYPE_OBJECT;
    }
    default:
        return ZG_TYPE_UNDEFINED;
    }
}

JSValueRef makeError(JSContextRef context, const std::string& message)
{
    JSStringHolder text(message);
    JSValueRef argument = JSValueMakeString(context, text.string);
    return JSObjectMakeError(context, 1, &argument, nullptr);
}

JSValueRef toJS(JSContextRef context, const zg_value* value, JSValueRef* exception)
{
    if (!value)
        return JSValueMakeUndefined(context);
    switch (value->kind) {
    case zg_value::Kind::Throw:
        *exception = makeError(context, value->text);
        return JSValueMakeUndefined(context);
    case zg_value::Kind::Promise:
        return value->promise;
    case zg_value::Kind::JSON: {
        JSStringHolder json(value->text);
        if (JSValueRef result = JSValueMakeFromJSONString(context, json.string))
            return result;
        *exception = makeError(context, "a native binding returned invalid JSON");
        return JSValueMakeUndefined(context);
    }
    case zg_value::Kind::Plain:
        break;
    }
    switch (value->type) {
    case ZG_TYPE_NULL:
        return JSValueMakeNull(context);
    case ZG_TYPE_BOOLEAN:
        return JSValueMakeBoolean(context, value->boolean);
    case ZG_TYPE_NUMBER:
        return JSValueMakeNumber(context, value->number);
    case ZG_TYPE_BIGINT:
        return JSBigIntCreateWithInt64(context, value->integer, exception);
    case ZG_TYPE_STRING: {
        JSStringHolder text(value->text);
        return JSValueMakeString(context, text.string);
    }
    default:
        return JSValueMakeUndefined(context);
    }
}

JSValueRef argument(const zg_call* call, size_t index)
{
    if (!call || index >= call->argc)
        return nullptr;
    return call->argv[index];
}

JSValueRef callBinding(JSContextRef context, zg_binding& binding, size_t argc, const JSValueRef argv[], JSValueRef* exception)
{
    auto* view = binding.view;
    if (!view) {
        *exception = makeError(context, "the view of this binding was destroyed");
        return JSValueMakeUndefined(context);
    }
    // The origin is checked again on every call: the document may have changed.
    RefPtr frame = view->view->page().localMainFrame();
    RefPtr document = frame ? frame->document() : nullptr;
    URL url = document ? document->url() : URL { };
    if (!document || !isTrusted(*view->context, url, ZG_TRUST_BINDINGS, true)) {
        *exception = makeError(context, "this origin may not call native bindings");
        return JSValueMakeUndefined(context);
    }

    zg_call call;
    call.context = context;
    call.argc = argc;
    call.argv = argv;
    call.view = view;
    auto origin = document->securityOrigin().toString().utf8();
    call.origin.assign(byteCast<char>(origin.data()), origin.length());
    zg_value* result = binding.function(&call, binding.user);
    return toJS(context, result, exception);
}

// A bound function is a JSC native function, as WebCore's DOM functions are,
// not a C API callback object: those release the JavaScript lock around every
// call (draining microtasks and handing heap access to the collector), which cost
// some 40 µs per call. The application's function runs with the lock held; the
// zg_* calls it may make take it again.
JSC::JSObject* makeBoundFunction(JSC::JSGlobalObject* globalObject, const std::shared_ptr<zg_binding>& binding, const String& name)
{
    auto& vm = globalObject->vm();
    return JSC::JSNativeStdFunction::create(vm, globalObject, 0, name, [binding](JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame) -> JSC::EncodedJSValue {
        auto& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        Vector<JSValueRef, 8> arguments(callFrame->argumentCount(), [&](size_t i) {
            return toRef(globalObject, callFrame->uncheckedArgument(i));
        });
        JSValueRef exception = nullptr;
        JSValueRef result = callBinding(toRef(globalObject), *binding, arguments.size(), arguments.span().data(), &exception);
        if (exception) {
            JSC::throwException(globalObject, scope, toJS(globalObject, exception));
            return JSC::JSValue::encode(JSC::jsUndefined());
        }
        return JSC::JSValue::encode(result ? toJS(globalObject, result) : JSC::jsUndefined());
    });
}

void install(JSContextRef context, const std::shared_ptr<zg_binding>& binding)
{
    JSObjectRef object = JSContextGetGlobalObject(context);
    std::string_view path = binding->path;
    while (true) {
        auto dot = path.find('.');
        std::string name { path.substr(0, dot) };
        JSStringHolder property(name);
        if (dot == std::string_view::npos) {
            auto* globalObject = toJS(context);
            JSC::JSLockHolder lock(globalObject);
            auto* function = makeBoundFunction(globalObject, binding, String::fromUTF8(name.c_str()));
            JSObjectSetProperty(context, object, property.string, toRef(function), kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, nullptr);
            return;
        }
        JSValueRef next = JSObjectGetProperty(context, object, property.string, nullptr);
        if (!next || !JSValueIsObject(context, next)) {
            next = JSObjectMake(context, nullptr, nullptr);
            JSObjectSetProperty(context, object, property.string, next, kJSPropertyAttributeNone, nullptr);
        }
        object = JSValueToObject(context, next, nullptr);
        path = path.substr(dot + 1);
    }
}

zg_value* callValue(zg_call* call)
{
    if (!call)
        return nullptr;
    call->values.push_back(std::make_unique<zg_value>());
    return call->values.back().get();
}

zg_view* checkedView(zg_view* view)
{
    if (!view) {
        fail(ZG_ERROR_INVALID_ARGUMENT, "no view"_s);
        return nullptr;
    }
    return checkThread() ? view : nullptr;
}

} // namespace

namespace ZeroG::API {

JSGlobalContextRef globalContext(LocalFrame& frame)
{
    return toGlobalRef(frame.script().globalObject(mainThreadNormalWorldSingleton()));
}

void installBindings(zg_view& view, LocalFrame& frame)
{
    RefPtr document = frame.document();
    if (view.bindings.empty() || !document || !isTrusted(*view.context, document->url(), ZG_TRUST_BINDINGS, true))
        return;
    auto context = globalContext(frame);
    for (auto& binding : view.bindings)
        install(context, binding);
}

} // namespace ZeroG::API

extern "C" {

zg_result zg_view_bind(zg_view* view, const char* path, zg_bind_fn function, void* user)
{
    clearError();
    if (!checkedView(view))
        return ZG_ERROR_INVALID_ARGUMENT;
    if (!path || !*path || !function || path[0] == '.' || std::string_view(path).back() == '.')
        return fail(ZG_ERROR_INVALID_ARGUMENT, "a binding needs a dotted path and a function"_s);
    auto binding = std::make_shared<zg_binding>(zg_binding { view, path, function, user });
    view->bindings.push_back(binding);
    // Into the current document too, if its origin is trusted.
    if (RefPtr frame = view->view->page().localMainFrame())
        installBindings(*view, *frame);
    return ZG_OK;
}

zg_result zg_view_emit(zg_view* view, const char* name, const char* json, size_t length)
{
    clearError();
    if (!checkedView(view))
        return ZG_ERROR_INVALID_ARGUMENT;
    if (!name)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no event name"_s);
    RefPtr frame = view->view->page().localMainFrame();
    if (!frame)
        return fail(ZG_ERROR_NOT_FOUND, "no document"_s);
    auto context = globalContext(*frame);
    JSStringHolder source("(function(name, detail) { window.dispatchEvent(new CustomEvent(name, { detail })); })");
    JSValueRef exception = nullptr;
    JSValueRef dispatch = JSEvaluateScript(context, source.string, nullptr, nullptr, 1, &exception);
    JSStringHolder eventName(name);
    JSValueRef detail = JSValueMakeUndefined(context);
    if (json && length) {
        JSStringHolder detailJSON(std::string(json, length));
        detail = JSValueMakeFromJSONString(context, detailJSON.string);
        if (!detail)
            return fail(ZG_ERROR_INVALID_ARGUMENT, "the event detail is not valid JSON"_s);
    }
    JSValueRef arguments[] = { JSValueMakeString(context, eventName.string), detail };
    JSObjectCallAsFunction(context, JSValueToObject(context, dispatch, nullptr), nullptr, 2, arguments, &exception);
    if (exception)
        return fail(ZG_ERROR_JS_EXCEPTION, String::fromUTF8(toUTF8(context, exception).c_str()));
    return ZG_OK;
}

// ---- Call arguments ----

size_t zg_call_argc(const zg_call* call)
{
    return call ? call->argc : 0;
}

zg_value_type zg_call_arg_type(const zg_call* call, size_t index)
{
    auto value = argument(call, index);
    return value ? typeOf(call->context, value) : ZG_TYPE_UNDEFINED;
}

int zg_call_arg_bool(zg_call* call, size_t index)
{
    auto value = argument(call, index);
    return value && JSValueToBoolean(call->context, value);
}

double zg_call_arg_number(zg_call* call, size_t index)
{
    auto value = argument(call, index);
    return value ? JSValueToNumber(call->context, value, nullptr) : std::nan("");
}

int64_t zg_call_arg_int64(zg_call* call, size_t index)
{
    auto value = argument(call, index);
    if (!value)
        return 0;
    if (JSValueIsBigInt(call->context, value))
        return JSValueToInt64(call->context, value, nullptr);
    // Truncated toward zero, saturating at the int64 range; NaN is 0.
    double number = JSValueToNumber(call->context, value, nullptr);
    if (std::isnan(number))
        return 0;
    if (number >= 9223372036854775808.0)
        return std::numeric_limits<int64_t>::max();
    if (number < -9223372036854775808.0)
        return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(number);
}

const char* zg_call_arg_string(zg_call* call, size_t index, size_t* length)
{
    auto value = argument(call, index);
    if (!value) {
        if (length)
            *length = 0;
        return nullptr;
    }
    call->strings.push_back(std::make_unique<std::string>(toUTF8(call->context, value)));
    if (length)
        *length = call->strings.back()->size();
    return call->strings.back()->c_str();
}

const char* zg_call_arg_json(zg_call* call, size_t index, size_t* length)
{
    auto value = argument(call, index);
    if (!value) {
        if (length)
            *length = 0;
        return nullptr;
    }
    call->strings.push_back(std::make_unique<std::string>(toJSON(call->context, value)));
    if (length)
        *length = call->strings.back()->size();
    return call->strings.back()->c_str();
}

zg_view* zg_call_view(const zg_call* call)
{
    return call ? call->view : nullptr;
}

const char* zg_call_origin(const zg_call* call)
{
    return call ? call->origin.c_str() : nullptr;
}

// ---- Return values ----

zg_value* zg_value_null(zg_call* call)
{
    auto* value = callValue(call);
    if (value)
        value->type = ZG_TYPE_NULL;
    return value;
}

zg_value* zg_value_bool(zg_call* call, int boolean)
{
    auto* value = callValue(call);
    if (value) {
        value->type = ZG_TYPE_BOOLEAN;
        value->boolean = boolean;
    }
    return value;
}

zg_value* zg_value_number(zg_call* call, double number)
{
    auto* value = callValue(call);
    if (value) {
        value->type = ZG_TYPE_NUMBER;
        value->number = number;
    }
    return value;
}

zg_value* zg_value_int64(zg_call* call, int64_t integer)
{
    auto* value = callValue(call);
    if (!value)
        return nullptr;
    // A Number while it is exact, a BigInt beyond 2^53.
    constexpr int64_t maximumSafeInteger = (int64_t { 1 } << 53) - 1;
    if (integer >= -maximumSafeInteger && integer <= maximumSafeInteger) {
        value->type = ZG_TYPE_NUMBER;
        value->number = static_cast<double>(integer);
    } else {
        value->type = ZG_TYPE_BIGINT;
        value->integer = integer;
    }
    return value;
}

zg_value* zg_value_string(zg_call* call, const char* text)
{
    return zg_value_string_len(call, text, text ? strlen(text) : 0);
}

zg_value* zg_value_string_len(zg_call* call, const char* text, size_t length)
{
    auto* value = callValue(call);
    if (value) {
        value->type = ZG_TYPE_STRING;
        value->text.assign(text ? text : "", text ? length : 0);
    }
    return value;
}

zg_value* zg_value_json(zg_call* call, const char* json, size_t length)
{
    auto* value = callValue(call);
    if (value) {
        value->kind = zg_value::Kind::JSON;
        value->text.assign(json ? json : "null", json ? length : 4);
    }
    return value;
}

zg_value* zg_call_throw(zg_call* call, const char* message)
{
    auto* value = callValue(call);
    if (value) {
        value->kind = zg_value::Kind::Throw;
        value->text = message ? message : "error";
    }
    return value;
}

// ---- Promises ----

zg_value* zg_call_promise(zg_call* call, zg_promise** out)
{
    if (out)
        *out = nullptr;
    auto* value = callValue(call);
    if (!value || !out)
        return value;
    JSObjectRef resolve = nullptr;
    JSObjectRef reject = nullptr;
    JSObjectRef promise = JSObjectMakeDeferredPromise(call->context, &resolve, &reject, nullptr);
    if (!promise) {
        value->kind = zg_value::Kind::Throw;
        value->text = "cannot create a Promise";
        return value;
    }
    auto* pending = new zg_promise;
    pending->context = JSGlobalContextRetain(JSContextGetGlobalContext(call->context));
    pending->resolve = resolve;
    pending->reject = reject;
    JSValueProtect(pending->context, resolve);
    JSValueProtect(pending->context, reject);
    value->kind = zg_value::Kind::Promise;
    value->promise = promise;
    *out = pending;
    return value;
}

static void settleNow(zg_promise* promise, bool resolved, const std::string& payload)
{
    auto context = promise->context;
    JSValueRef argument;
    if (resolved) {
        JSStringHolder json(payload.empty() ? std::string("null") : payload);
        argument = JSValueMakeFromJSONString(context, json.string);
        if (!argument) {
            argument = makeError(context, "a native binding resolved a Promise with invalid JSON");
            resolved = false;
        }
    } else
        argument = makeError(context, payload);
    JSObjectCallAsFunction(context, resolved ? promise->resolve : promise->reject, nullptr, 1, &argument, nullptr);
    JSValueUnprotect(context, promise->resolve);
    JSValueUnprotect(context, promise->reject);
    JSGlobalContextRelease(context);
    delete promise;
}

// Settled on the runtime thread: at once there (the page's continuation runs as a
// microtask, as for any resolved Promise), else through the engine's run loop.
static void settle(zg_promise* promise, bool resolved, std::string&& payload)
{
    if (isMainThread()) {
        settleNow(promise, resolved, payload);
        return;
    }
    callOnMainThread([promise, resolved, payload = WTF::move(payload)] {
        settleNow(promise, resolved, payload);
    });
}

void zg_promise_resolve(zg_promise* promise, const char* json, size_t length)
{
    if (!promise || promise->settled.exchange(true))
        return;
    settle(promise, true, json ? std::string(json, length) : std::string());
}

void zg_promise_reject(zg_promise* promise, const char* message)
{
    if (!promise || promise->settled.exchange(true))
        return;
    settle(promise, false, message ? message : "rejected");
}

// ---- zg_view_evaluate and its values ----

zg_result zg_view_evaluate(zg_view* view, const char* script, size_t length, zg_value** result)
{
    clearError();
    if (result)
        *result = nullptr;
    if (!checkedView(view))
        return ZG_ERROR_INVALID_ARGUMENT;
    if (!script)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no script"_s);
    RefPtr frame = view->view->page().localMainFrame();
    if (!frame)
        return fail(ZG_ERROR_NOT_FOUND, "no document"_s);
    auto context = globalContext(*frame);
    JSStringHolder source(std::string(script, length));
    JSValueRef exception = nullptr;
    JSValueRef value = JSEvaluateScript(context, source.string, nullptr, nullptr, 1, &exception);
    if (exception)
        return fail(ZG_ERROR_JS_EXCEPTION, String::fromUTF8(toUTF8(context, exception).c_str()));
    if (result) {
        auto* owned = new zg_value;
        owned->kind = zg_value::Kind::JSON;
        owned->type = typeOf(context, value);
        owned->text = toJSON(context, value);
        *result = owned;
    }
    return ZG_OK;
}

zg_value_type zg_value_type_of(const zg_value* value)
{
    return value ? value->type : ZG_TYPE_UNDEFINED;
}

const char* zg_value_to_json(zg_value* value, size_t* length)
{
    if (!value) {
        if (length)
            *length = 0;
        return nullptr;
    }
    if (value->kind != zg_value::Kind::JSON) {
        // A value made for a call: its JSON on demand.
        switch (value->type) {
        case ZG_TYPE_NULL:
            value->text = "null";
            break;
        case ZG_TYPE_BOOLEAN:
            value->text = value->boolean ? "true" : "false";
            break;
        case ZG_TYPE_NUMBER: {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.17g", value->number);
            value->text = std::isfinite(value->number) ? buffer : "null";
            break;
        }
        case ZG_TYPE_BIGINT:
            value->text = std::to_string(value->integer);
            break;
        case ZG_TYPE_STRING: {
            std::string quoted = "\"";
            for (char c : value->text) {
                if (c == '"' || c == '\\')
                    quoted += '\\';
                quoted += c;
            }
            value->text = quoted + "\"";
            break;
        }
        default:
            value->text = "null";
        }
        value->kind = zg_value::Kind::JSON;
    }
    if (length)
        *length = value->text.size();
    return value->text.c_str();
}

void zg_value_release(zg_value* value)
{
    // Values from zg_view_evaluate; those made for a call belong to the call.
    delete value;
}

} // extern "C"
