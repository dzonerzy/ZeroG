// Direct JavaScript bindings (docs/design/c-api.md, "Direct JavaScript bindings"): native functions and classes
// installed on the main frame's window object of trusted origins, called
// synchronously as JSC native functions (no IPC), with Promises settled later
// from any thread; references to JavaScript values the host keeps; scripts run
// with the bindings. Also the values of zg_view_evaluate and zg_view_emit.

#include "config.h"
#include "APIInternal.h"

#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/CallFrameInlines.h>
#include <JavaScriptCore/GetterSetter.h>
#include <JavaScriptCore/InternalFunction.h>
#include <JavaScriptCore/JSArrayBuffer.h>
#include <JavaScriptCore/JSArrayBufferView.h>
#include <JavaScriptCore/JSCJSValueInlines.h>
#include <JavaScriptCore/JSCast.h>
#include <JavaScriptCore/JSCellInlines.h>
#include <JavaScriptCore/JSDestructibleObject.h>
#include <JavaScriptCore/JSNativeStdFunction.h>
#include <JavaScriptCore/JSObjectInlines.h>
#include <JavaScriptCore/ObjectConstructor.h>
#include <JavaScriptCore/OpaqueJSString.h>
#include <JavaScriptCore/JSContextRef.h>
#include <JavaScriptCore/JSObjectRef.h>
#include <JavaScriptCore/JSStringRef.h>
#include <JavaScriptCore/JSTypedArray.h>
#include <JavaScriptCore/JSValueRef.h>
#include <JavaScriptCore/Weak.h>
#include <JavaScriptCore/WeakInlines.h>
#include <WebCore/DOMWindow.h>
#include <WebCore/DOMWrapperWorld.h>
#include <WebCore/Document.h>
#include <WebCore/JSDOMGlobalObject.h>
#include <WebCore/JSDOMWindowBase.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/Page.h>
#include <WebCore/ScriptController.h>
#include <atomic>
#include <cmath>
#include <wtf/HashSet.h>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

using namespace WebCore;
using namespace ZeroG;
using namespace ZeroG::API;

namespace {

class Instance;

// A JavaScript value the host keeps (zg_ref), and values that return it.
struct HeldValue {
    HeldValue(JSContextRef context, JSValueRef value)
        : context(JSGlobalContextRetain(JSContextGetGlobalContext(context)))
        , value(value)
    {
        JSValueProtect(this->context, value);
    }
    ~HeldValue()
    {
        JSValueUnprotect(context, value);
        JSGlobalContextRelease(context);
    }
    JSGlobalContextRef context;
    JSValueRef value;
};

// What a class's instances and functions keep: no JSC handles, so that it can go
// with the last of them, during a collection.
struct ClassCore {
    zg_view* view { nullptr }; // cleared when the view is destroyed
    std::string name;
    zg_bind_fn constructor { nullptr };
    zg_finalize_fn finalize { nullptr };
    void* user { nullptr };
    HashSet<Instance*> live; // runtime thread
};

struct ClassMember {
    std::string name;
    zg_bind_fn method { nullptr };
    zg_bind_fn getter { nullptr };
    zg_bind_fn setter { nullptr };
    void* user { nullptr };
};

} // namespace

struct zg_value {
    enum class Kind : uint8_t { Plain, JSON, Throw, Promise, Bytes, Ref, Instance };
    ~zg_value()
    {
        // In-place bytes that never reached JavaScript go back to their owner.
        if (kind == Kind::Bytes && external && release)
            release(releaseUser);
    }
    Kind kind { Kind::Plain };
    zg_value_type type { ZG_TYPE_UNDEFINED };
    bool boolean { false };
    double number { 0 };
    int64_t integer { 0 };
    std::string text; // string, JSON, or the message to throw
    JSObjectRef promise { nullptr };
    // Kind::Bytes: a copy, or the application's memory in place.
    std::vector<uint8_t> bytes;
    void* external { nullptr };
    size_t externalLength { 0 };
    zg_release_fn release { nullptr };
    void* releaseUser { nullptr };
    std::shared_ptr<HeldValue> held; // Kind::Ref
    zg_class* instanceClass { nullptr }; // Kind::Instance
    void* native { nullptr };
};

struct zg_call {
    JSContextRef context { nullptr };
    size_t argc { 0 };
    const JSValueRef* argv { nullptr };
    zg_view* view { nullptr };
    std::string origin;
    Instance* instance { nullptr }; // `this` of a class's constructor, method or accessor
    bool constructing { false };
    std::vector<std::unique_ptr<std::string>> strings; // arguments read as text
    std::vector<std::unique_ptr<zg_value>> values; // returned values, freed after the call
};

struct zg_promise {
    JSGlobalContextRef context { nullptr };
    JSObjectRef resolve { nullptr };
    JSObjectRef reject { nullptr };
    std::atomic<bool> settled { false };
};

struct zg_ref {
    std::shared_ptr<HeldValue> held;
};

// Owned by its view. Per page (global object), the prototype and the structure of
// its instances, held weakly: they go with the page.
struct zg_class {
    struct Realm {
        JSC::Weak<JSC::JSObject> prototype;
        JSC::Weak<JSC::Structure> structure;
    };
    std::shared_ptr<ClassCore> core;
    std::string path;
    std::vector<ClassMember> members;
    HashMap<JSC::JSGlobalObject*, std::unique_ptr<Realm>> realms;
};

namespace {

// CREATE_METHOD_TABLE names it unqualified, as code inside namespace JSC would.
namespace JSCastingHelpers = JSC::JSCastingHelpers;

// An instance of a class: a JavaScript object with the application's pointer.
class Instance final : public JSC::JSDestructibleObject {
public:
    using Base = JSC::JSDestructibleObject;

    template<typename CellType, JSC::SubspaceAccess>
    static JSC::CompleteSubspace* subspaceFor(JSC::VM& vm)
    {
        return &vm.destructibleObjectSpace();
    }

    static JSC::Structure* createStructure(JSC::VM& vm, JSC::JSGlobalObject* globalObject, JSC::JSValue prototype)
    {
        return JSC::Structure::create(vm, globalObject, prototype, JSC::TypeInfo(JSC::ObjectType, StructureFlags), info());
    }

    static Instance* create(JSC::VM& vm, JSC::Structure* structure, const std::shared_ptr<ClassCore>& core, void* native)
    {
        auto* instance = new (NotNull, JSC::allocateCell<Instance>(vm)) Instance(vm, structure, core, native);
        instance->finishCreation(vm);
        core->live.add(instance);
        return instance;
    }

    static void destroy(JSC::JSCell* cell)
    {
        static_cast<Instance*>(cell)->~Instance();
    }

    DECLARE_INFO;

    ClassCore& core() const { return *m_core; }
    void* native() const { return m_native; }
    void setNative(void* native) { m_native = native; }

private:
    Instance(JSC::VM& vm, JSC::Structure* structure, const std::shared_ptr<ClassCore>& core, void* native)
        : Base(vm, structure)
        , m_core(core)
        , m_native(native)
    {
    }

    // During a sweep: the application's finalizer runs afterwards, when it may
    // call the API.
    ~Instance()
    {
        m_core->live.remove(this);
        if (m_native && m_core->finalize) {
            callOnMainThread([finalize = m_core->finalize, native = m_native, user = m_core->user] {
                finalize(native, user);
            });
        }
    }

    std::shared_ptr<ClassCore> m_core;
    void* m_native;
};

const JSC::ClassInfo Instance::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Instance) };

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

// The bytes of an ArrayBuffer, typed array or DataView, without pinning the
// buffer (the C API's accessors pin it for good).
std::optional<std::span<uint8_t>> bytesOf(JSContextRef context, JSValueRef value)
{
    auto jsValue = toJS(toJS(context), value);
    if (auto* view = dynamicDowncast<JSC::JSArrayBufferView>(jsValue)) {
        if (view->isDetached())
            return std::nullopt;
        return std::span { static_cast<uint8_t*>(view->vector()), view->byteLength() };
    }
    if (auto* buffer = dynamicDowncast<JSC::JSArrayBuffer>(jsValue)) {
        auto* impl = buffer->impl();
        if (!impl || impl->isDetached())
            return std::nullopt;
        return std::span { static_cast<uint8_t*>(impl->data()), impl->byteLength() };
    }
    return std::nullopt;
}

bool isBytes(JSContextRef context, JSValueRef value)
{
    auto jsValue = toJS(toJS(context), value);
    return dynamicDowncast<JSC::JSArrayBufferView>(jsValue) || dynamicDowncast<JSC::JSArrayBuffer>(jsValue);
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
        if (isBytes(context, value))
            return ZG_TYPE_BYTES;
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

// A Uint8Array over `data`, which `deallocate` frees once the buffer is collected.
JSValueRef makeUint8Array(JSContextRef context, void* data, size_t length, JSTypedArrayBytesDeallocator deallocate, void* deallocatorContext, JSValueRef* exception)
{
    JSObjectRef buffer = JSObjectMakeArrayBufferWithBytesNoCopy(context, data, length, deallocate, deallocatorContext, exception);
    if (!buffer)
        return JSValueMakeUndefined(context);
    return JSObjectMakeTypedArrayWithArrayBuffer(context, kJSTypedArrayTypeUint8Array, buffer, exception);
}

JSValueRef copyToUint8Array(JSContextRef context, std::span<const uint8_t> bytes, JSValueRef* exception)
{
    void* copy = fastMalloc(std::max<size_t>(bytes.size(), 1));
    memcpySpan(std::span { static_cast<uint8_t*>(copy), bytes.size() }, bytes);
    return makeUint8Array(context, copy, bytes.size(), [](void* data, void*) { fastFree(data); }, nullptr, exception);
}

JSValueRef newInstance(JSContextRef, zg_class&, void* native, JSValueRef* exception);

JSValueRef valueToJS(JSContextRef context, zg_value* value, JSValueRef* exception)
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
    case zg_value::Kind::Bytes:
        if (value->external) {
            // Once only: JavaScript owns the memory from here.
            auto release = std::exchange(value->release, nullptr);
            auto* user = value->releaseUser;
            auto* data = std::exchange(value->external, nullptr);
            struct Release { zg_release_fn release; void* user; };
            auto* owner = new Release { release, user };
            return makeUint8Array(context, data, value->externalLength, [](void*, void* context) {
                auto* owner = static_cast<Release*>(context);
                if (owner->release)
                    owner->release(owner->user);
                delete owner;
            }, owner, exception);
        }
        if (value->release) {
            *exception = makeError(context, "in-place bytes can be passed to JavaScript once");
            return JSValueMakeUndefined(context);
        }
        return copyToUint8Array(context, value->bytes, exception);
    case zg_value::Kind::Ref:
        // A page's objects stay in that page.
        if (value->held->context != JSContextGetGlobalContext(context)) {
            *exception = makeError(context, "a reference can only go back to the page it came from");
            return JSValueMakeUndefined(context);
        }
        return value->held->value;
    case zg_value::Kind::Instance:
        return newInstance(context, *value->instanceClass, value->native, exception);
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

// Calls one of the application's functions for JavaScript: a bound function, a
// class's constructor, method or accessor.
JSValueRef invoke(JSContextRef context, zg_view* view, zg_bind_fn function, void* user, size_t argc, const JSValueRef argv[], JSValueRef* exception, Instance* instance = nullptr, bool constructing = false)
{
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
    call.instance = instance;
    call.constructing = constructing;
    auto origin = document->securityOrigin().toString().utf8();
    call.origin.assign(byteCast<char>(origin.data()), origin.length());
    zg_value* result = function(&call, user);
    return valueToJS(context, result, exception);
}

// Runs a host call for a JSC native function: arguments in, exception out.
template<typename Call>
JSC::EncodedJSValue hostCall(JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame, const Call& call)
{
    auto& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<JSValueRef, 8> arguments(callFrame->argumentCount(), [&](size_t i) {
        return toRef(globalObject, callFrame->uncheckedArgument(i));
    });
    JSValueRef exception = nullptr;
    JSValueRef result = call(toRef(globalObject), arguments.size(), arguments.span().data(), &exception);
    if (exception) {
        JSC::throwException(globalObject, scope, toJS(globalObject, exception));
        return JSC::JSValue::encode(JSC::jsUndefined());
    }
    return JSC::JSValue::encode(result ? toJS(globalObject, result) : JSC::jsUndefined());
}

JSC::EncodedJSValue throwTypeError(JSC::JSGlobalObject* globalObject, const String& message)
{
    auto& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return JSC::throwVMTypeError(globalObject, scope, message);
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
        return hostCall(globalObject, callFrame, [&](JSContextRef context, size_t argc, const JSValueRef* argv, JSValueRef* exception) {
            return invoke(context, binding->view, binding->function, binding->user, argc, argv, exception);
        });
    });
}

// The object `path` names without its last segment, created as needed on
// `window`; the last segment goes to `name`.
JSObjectRef parentObject(JSContextRef context, std::string_view path, std::string& name)
{
    JSObjectRef object = JSContextGetGlobalObject(context);
    while (true) {
        auto dot = path.find('.');
        if (dot == std::string_view::npos) {
            name = path;
            return object;
        }
        JSStringHolder property(std::string { path.substr(0, dot) });
        JSValueRef next = JSObjectGetProperty(context, object, property.string, nullptr);
        if (!next || !JSValueIsObject(context, next)) {
            next = JSObjectMake(context, nullptr, nullptr);
            JSObjectSetProperty(context, object, property.string, next, kJSPropertyAttributeNone, nullptr);
        }
        object = JSValueToObject(context, next, nullptr);
        path = path.substr(dot + 1);
    }
}

void install(JSContextRef context, const std::shared_ptr<zg_binding>& binding)
{
    std::string name;
    JSObjectRef parent = parentObject(context, binding->path, name);
    auto* globalObject = toJS(context);
    JSC::JSLockHolder lock(globalObject);
    auto* function = makeBoundFunction(globalObject, binding, String::fromUTF8(name.c_str()));
    JSStringHolder property(name);
    JSObjectSetProperty(context, parent, property.string, toRef(function), kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, nullptr);
}

// ---- Classes ----

thread_local bool t_constructing;

// `new` on a class constructor: its function runs in construct mode.
JSC_DEFINE_HOST_FUNCTION(constructClass, (JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame))
{
    auto* callee = uncheckedDowncast<JSC::JSNativeStdFunction>(callFrame->jsCallee());
    t_constructing = true;
    return callee->function()(globalObject, callFrame);
}

zg_class::Realm* realmFor(zg_class& cls, JSC::JSGlobalObject* globalObject)
{
    auto iterator = cls.realms.find(globalObject);
    if (iterator == cls.realms.end() || !iterator->value->prototype)
        return nullptr;
    return iterator->value.get();
}

// The structure of the class's instances in this page, made again if it was
// collected while the prototype lived on.
JSC::Structure* instanceStructure(zg_class& cls, JSC::JSGlobalObject* globalObject)
{
    auto* realm = realmFor(cls, globalObject);
    if (!realm)
        return nullptr;
    if (!realm->structure) {
        auto& vm = globalObject->vm();
        realm->structure = JSC::Weak<JSC::Structure>(Instance::createStructure(vm, globalObject, realm->prototype.get()));
    }
    return realm->structure.get();
}

JSValueRef newInstance(JSContextRef context, zg_class& cls, void* native, JSValueRef* exception)
{
    auto* globalObject = toJS(context);
    JSC::JSLockHolder lock(globalObject);
    auto* structure = instanceStructure(cls, globalObject);
    if (!structure) {
        *exception = makeError(context, "the class " + cls.path + " is not installed in this page");
        return JSValueMakeUndefined(context);
    }
    return toRef(globalObject, Instance::create(globalObject->vm(), structure, cls.core, native));
}

// `this` of a method or accessor, if an instance of the class.
Instance* thisInstance(JSC::CallFrame* callFrame, const ClassCore& core)
{
    auto* instance = dynamicDowncast<Instance>(callFrame->thisValue());
    return instance && &instance->core() == &core ? instance : nullptr;
}

JSC::JSObject* makeMember(JSC::JSGlobalObject* globalObject, const std::shared_ptr<ClassCore>& core, zg_bind_fn function, void* user, const String& name, unsigned length)
{
    auto& vm = globalObject->vm();
    return JSC::JSNativeStdFunction::create(vm, globalObject, length, name, [core, function, user](JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame) -> JSC::EncodedJSValue {
        auto* instance = thisInstance(callFrame, *core);
        if (!instance)
            return throwTypeError(globalObject, makeString("not an instance of "_s, String::fromUTF8(core->name.c_str())));
        return hostCall(globalObject, callFrame, [&](JSContextRef context, size_t argc, const JSValueRef* argv, JSValueRef* exception) {
            return invoke(context, core->view, function, user, argc, argv, exception, instance);
        });
    });
}

void defineMember(JSC::JSGlobalObject* globalObject, JSC::JSObject* prototype, const std::shared_ptr<ClassCore>& core, const ClassMember& member)
{
    auto& vm = globalObject->vm();
    auto name = String::fromUTF8(member.name.c_str());
    auto identifier = JSC::Identifier::fromString(vm, name);
    if (member.method) {
        prototype->putDirect(vm, identifier, makeMember(globalObject, core, member.method, member.user, name, 0), static_cast<unsigned>(JSC::PropertyAttribute::DontEnum));
        return;
    }
    auto* getter = member.getter ? makeMember(globalObject, core, member.getter, member.user, makeString("get "_s, name), 0) : nullptr;
    auto* setter = member.setter ? makeMember(globalObject, core, member.setter, member.user, makeString("set "_s, name), 1) : nullptr;
    prototype->putDirectAccessor(globalObject, identifier, JSC::GetterSetter::create(vm, globalObject, getter, setter), JSC::PropertyAttribute::Accessor | JSC::PropertyAttribute::DontEnum);
}

void installClass(JSContextRef context, const std::shared_ptr<zg_class>& cls)
{
    std::string name;
    JSObjectRef parent = parentObject(context, cls->path, name);
    auto* globalObject = toJS(context);
    auto& vm = globalObject->vm();
    JSC::JSLockHolder lock(vm);
    auto className = String::fromUTF8(name.c_str());

    auto* prototype = JSC::constructEmptyObject(globalObject);
    std::weak_ptr<zg_class> weakClass = cls;
    auto core = cls->core;
    auto* constructor = JSC::JSNativeStdFunction::create(vm, globalObject, 0, className, [weakClass, core](JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame) -> JSC::EncodedJSValue {
        if (!std::exchange(t_constructing, false))
            return throwTypeError(globalObject, makeString("class constructor "_s, String::fromUTF8(core->name.c_str()), " cannot be invoked without 'new'"_s));
        auto cls = weakClass.lock();
        if (!cls || !core->view)
            return throwTypeError(globalObject, "the view of this class was destroyed"_s);
        if (!core->constructor)
            return throwTypeError(globalObject, makeString("illegal constructor: "_s, String::fromUTF8(core->name.c_str()), " instances come from the application"_s));
        auto& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        auto* structure = instanceStructure(*cls, globalObject);
        if (!structure)
            return JSC::throwVMTypeError(globalObject, scope, "the class is not installed in this page"_s);
        // `class Sub extends App.File`: the instance gets the subclass's prototype.
        auto newTarget = callFrame->newTarget();
        if (newTarget.isObject() && JSC::asObject(newTarget) != callFrame->jsCallee()) {
            structure = JSC::InternalFunction::createSubclassStructure(globalObject, JSC::asObject(newTarget), structure);
            RETURN_IF_EXCEPTION(scope, { });
        }
        auto* instance = Instance::create(vm, structure, core, nullptr);
        Vector<JSValueRef, 8> arguments(callFrame->argumentCount(), [&](size_t i) {
            return toRef(globalObject, callFrame->uncheckedArgument(i));
        });
        JSValueRef exception = nullptr;
        invoke(toRef(globalObject), core->view, core->constructor, core->user, arguments.size(), arguments.span().data(), &exception, instance, true);
        if (exception)
            return JSC::throwVMError(globalObject, scope, toJS(globalObject, exception));
        // The constructor's own return value is not the object made.
        return JSC::JSValue::encode(instance);
    }, JSC::NoIntrinsic, JSC::NativeFunction(constructClass));

    auto attributes = JSC::PropertyAttribute::DontEnum | JSC::PropertyAttribute::DontDelete | JSC::PropertyAttribute::ReadOnly;
    constructor->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(attributes));
    prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(JSC::PropertyAttribute::DontEnum));
    prototype->putDirect(vm, vm.propertyNames->toStringTagSymbol, JSC::jsString(vm, className), static_cast<unsigned>(JSC::PropertyAttribute::DontEnum | JSC::PropertyAttribute::ReadOnly));
    for (auto& member : cls->members)
        defineMember(globalObject, prototype, core, member);

    // Pages that are gone leave their entries behind; a new page may reuse the
    // address of an old one.
    cls->realms.removeIf([](auto& entry) { return !entry.value->prototype; });
    auto realm = std::make_unique<zg_class::Realm>();
    realm->prototype = JSC::Weak<JSC::JSObject>(prototype);
    realm->structure = JSC::Weak<JSC::Structure>(Instance::createStructure(vm, globalObject, prototype));
    cls->realms.set(globalObject, WTF::move(realm));

    JSStringHolder property(name);
    JSObjectSetProperty(context, parent, property.string, toRef(constructor), kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, nullptr);
}

// ---- Page scripts ----

void runScript(LocalFrame& frame, const std::string& source)
{
    // Errors are reported to the page's console, and so to js_error.
    frame.script().executeScriptIgnoringException(String::fromUTF8ReplacingInvalidSequences(byteCast<char8_t>(std::span { source.data(), source.size() })), JSC::SourceTaintedOrigin::Untainted);
}

zg_value* callValue(zg_call* call)
{
    // Without a call: a value the caller owns.
    if (!call)
        return new zg_value;
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

// The main frame, if its document gets the bindings.
LocalFrame* trustedFrame(zg_view& view)
{
    RefPtr frame = view.view->page().localMainFrame();
    RefPtr document = frame ? frame->document() : nullptr;
    if (!document || !isTrusted(*view.context, document->url(), ZG_TRUST_BINDINGS, true))
        return nullptr;
    return frame.get();
}

bool validPath(const char* path)
{
    return path && *path && path[0] != '.' && std::string_view(path).back() != '.';
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
    if ((view.bindings.empty() && view.classes.empty() && view.scripts.empty()) || !document || !isTrusted(*view.context, document->url(), ZG_TRUST_BINDINGS, true))
        return;
    auto context = globalContext(frame);
    for (auto& binding : view.bindings)
        install(context, binding);
    for (auto& cls : view.classes)
        installClass(context, cls);
    for (auto& script : view.scripts)
        runScript(frame, script);
}

void detachBindings(zg_view& view)
{
    // Functions bound in pages still referenced elsewhere throw from now on.
    for (auto& binding : view.bindings)
        binding->view = nullptr;
    // Instances outlive the view in pages not yet collected: their pointers are
    // finalized now, once.
    for (auto& cls : view.classes) {
        auto& core = *cls->core;
        core.view = nullptr;
        for (auto* instance : copyToVector(core.live)) {
            if (void* native = instance->native()) {
                instance->setNative(nullptr);
                if (core.finalize)
                    core.finalize(native, core.user);
            }
        }
    }
}

} // namespace ZeroG::API

extern "C" {

zg_result zg_view_bind(zg_view* view, const char* path, zg_bind_fn function, void* user)
{
    clearError();
    if (!checkedView(view))
        return ZG_ERROR_INVALID_ARGUMENT;
    if (!validPath(path) || !function)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "a binding needs a dotted path and a function"_s);
    auto binding = std::make_shared<zg_binding>(zg_binding { view, path, function, user });
    view->bindings.push_back(binding);
    // Into the current document too, if its origin is trusted.
    if (auto* frame = trustedFrame(*view))
        install(globalContext(*frame), binding);
    return ZG_OK;
}

zg_result zg_view_add_script(zg_view* view, const char* source, size_t length)
{
    clearError();
    if (!checkedView(view))
        return ZG_ERROR_INVALID_ARGUMENT;
    if (!source)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no script"_s);
    view->scripts.emplace_back(source, length);
    if (auto* frame = trustedFrame(*view))
        runScript(*frame, view->scripts.back());
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

void* zg_call_arg_bytes(zg_call* call, size_t index, size_t* length)
{
    if (length)
        *length = 0;
    auto value = argument(call, index);
    if (!value)
        return nullptr;
    JSC::JSLockHolder lock(toJS(call->context));
    auto bytes = bytesOf(call->context, value);
    if (!bytes)
        return nullptr;
    if (length)
        *length = bytes->size();
    // A pointer even for an empty buffer, so that NULL means "not bytes".
    static uint8_t empty;
    return bytes->empty() ? &empty : bytes->data();
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
    value->type = ZG_TYPE_NULL;
    return value;
}

zg_value* zg_value_bool(zg_call* call, int boolean)
{
    auto* value = callValue(call);
    value->type = ZG_TYPE_BOOLEAN;
    value->boolean = boolean;
    return value;
}

zg_value* zg_value_number(zg_call* call, double number)
{
    auto* value = callValue(call);
    value->type = ZG_TYPE_NUMBER;
    value->number = number;
    return value;
}

zg_value* zg_value_int64(zg_call* call, int64_t integer)
{
    auto* value = callValue(call);
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
    value->type = ZG_TYPE_STRING;
    value->text.assign(text ? text : "", text ? length : 0);
    return value;
}

zg_value* zg_value_json(zg_call* call, const char* json, size_t length)
{
    auto* value = callValue(call);
    value->kind = zg_value::Kind::JSON;
    value->text.assign(json ? json : "null", json ? length : 4);
    return value;
}

zg_value* zg_value_bytes(zg_call* call, const void* data, size_t length)
{
    auto* value = callValue(call);
    value->kind = zg_value::Kind::Bytes;
    value->type = ZG_TYPE_BYTES;
    if (data && length)
        value->bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + length);
    return value;
}

zg_value* zg_value_bytes_external(zg_call* call, void* data, size_t length, zg_release_fn release, void* user)
{
    if (!data)
        return zg_value_bytes(call, nullptr, 0);
    auto* value = callValue(call);
    value->kind = zg_value::Kind::Bytes;
    value->type = ZG_TYPE_BYTES;
    value->external = data;
    value->externalLength = length;
    value->release = release;
    value->releaseUser = user;
    return value;
}

zg_value* zg_call_throw(zg_call* call, const char* message)
{
    if (!call)
        return nullptr;
    auto* value = callValue(call);
    value->kind = zg_value::Kind::Throw;
    value->text = message ? message : "error";
    return value;
}

// ---- Promises ----

zg_value* zg_call_promise(zg_call* call, zg_promise** out)
{
    if (out)
        *out = nullptr;
    if (!call)
        return nullptr;
    auto* value = callValue(call);
    if (!out)
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

enum class Settlement : uint8_t { JSON, Bytes, Reject };

static void settleNow(zg_promise* promise, Settlement settlement, const std::string& payload)
{
    auto context = promise->context;
    JSValueRef argument;
    bool resolved = settlement != Settlement::Reject;
    if (settlement == Settlement::JSON) {
        JSStringHolder json(payload.empty() ? std::string("null") : payload);
        argument = JSValueMakeFromJSONString(context, json.string);
        if (!argument) {
            argument = makeError(context, "a native binding resolved a Promise with invalid JSON");
            resolved = false;
        }
    } else if (settlement == Settlement::Bytes) {
        JSValueRef exception = nullptr;
        argument = copyToUint8Array(context, byteCast<uint8_t>(std::span { payload.data(), payload.size() }), &exception);
        if (exception) {
            argument = exception;
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
static void settle(zg_promise* promise, Settlement settlement, std::string&& payload)
{
    if (isMainThread()) {
        settleNow(promise, settlement, payload);
        return;
    }
    callOnMainThread([promise, settlement, payload = WTF::move(payload)] {
        settleNow(promise, settlement, payload);
    });
}

void zg_promise_resolve(zg_promise* promise, const char* json, size_t length)
{
    if (!promise || promise->settled.exchange(true))
        return;
    settle(promise, Settlement::JSON, json ? std::string(json, length) : std::string());
}

void zg_promise_resolve_bytes(zg_promise* promise, const void* data, size_t length)
{
    if (!promise || promise->settled.exchange(true))
        return;
    settle(promise, Settlement::Bytes, data ? std::string(static_cast<const char*>(data), length) : std::string());
}

void zg_promise_reject(zg_promise* promise, const char* message)
{
    if (!promise || promise->settled.exchange(true))
        return;
    settle(promise, Settlement::Reject, message ? message : "rejected");
}

// ---- References ----

zg_ref* zg_call_arg_ref(zg_call* call, size_t index)
{
    auto value = argument(call, index);
    if (!value)
        return nullptr;
    return new zg_ref { std::make_shared<HeldValue>(call->context, value) };
}

zg_value* zg_value_ref(zg_call* call, const zg_ref* ref)
{
    if (!ref)
        return zg_value_null(call);
    auto* value = callValue(call);
    value->kind = zg_value::Kind::Ref;
    value->type = typeOf(ref->held->context, ref->held->value);
    value->held = ref->held;
    return value;
}

zg_value_type zg_ref_type(const zg_ref* ref)
{
    if (!ref || !checkThread())
        return ZG_TYPE_UNDEFINED;
    return typeOf(ref->held->context, ref->held->value);
}

zg_result zg_ref_call(zg_ref* function, size_t argc, zg_value* const* argv, zg_value** result)
{
    clearError();
    if (result)
        *result = nullptr;
    if (!function)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no function"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    auto context = function->held->context;
    auto* globalObject = toJS(context);
    {
        // A detached page runs no script.
        JSC::JSLockHolder lock(globalObject);
        auto* window = dynamicDowncast<JSDOMWindowBase>(globalObject);
        if (!window || !window->wrapped().frame())
            return fail(ZG_ERROR_NOT_FOUND, "the page of this function is gone"_s);
    }
    JSObjectRef object = JSValueIsObject(context, function->held->value) ? JSValueToObject(context, function->held->value, nullptr) : nullptr;
    if (!object || !JSObjectIsFunction(context, object))
        return fail(ZG_ERROR_INVALID_ARGUMENT, "the reference is not a function"_s);

    Vector<JSValueRef, 8> arguments;
    for (size_t i = 0; i < argc; ++i) {
        JSValueRef exception = nullptr;
        arguments.append(valueToJS(context, argv ? argv[i] : nullptr, &exception));
        if (exception)
            return fail(ZG_ERROR_INVALID_ARGUMENT, makeString("argument "_s, i, ": "_s, String::fromUTF8(toUTF8(context, exception).c_str())));
    }
    JSValueRef exception = nullptr;
    JSValueRef value = JSObjectCallAsFunction(context, object, nullptr, arguments.size(), arguments.span().data(), &exception);
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

void zg_ref_release(zg_ref* ref)
{
    if (!ref)
        return;
    // The value is unprotected on the runtime thread.
    ensureOnMainThread([ref] {
        delete ref;
    });
}

// ---- A page's objects from native code ----

static const char* typeName(zg_value_type type)
{
    switch (type) {
    case ZG_TYPE_UNDEFINED: return "undefined";
    case ZG_TYPE_NULL: return "null";
    case ZG_TYPE_BOOLEAN: return "a boolean";
    case ZG_TYPE_NUMBER: return "a number";
    case ZG_TYPE_BIGINT: return "a BigInt";
    case ZG_TYPE_STRING: return "a string";
    case ZG_TYPE_ARRAY: return "an array";
    case ZG_TYPE_OBJECT: return "an object";
    case ZG_TYPE_FUNCTION: return "a function";
    case ZG_TYPE_BYTES: return "bytes";
    }
    return "a value";
}

// The reference's context, if its page is still there to run script.
static zg_result referenceContext(const zg_ref* ref, JSContextRef& context)
{
    if (!ref)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no reference"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    context = ref->held->context;
    auto* globalObject = toJS(ref->held->context);
    JSC::JSLockHolder lock(globalObject);
    auto* window = dynamicDowncast<JSDOMWindowBase>(globalObject);
    if (!window || !window->wrapped().frame())
        return fail(ZG_ERROR_NOT_FOUND, "the page of this reference is gone"_s);
    return ZG_OK;
}

// Walks `name`, a property or a dotted path of them, from the reference's value
// to the object that holds its last step, and gives that step.
static zg_result resolvePath(const zg_ref* ref, const char* name, JSContextRef& context, JSObjectRef& holder, std::string& last)
{
    if (!name || !*name)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no property name"_s);
    if (auto result = referenceContext(ref, context); result != ZG_OK)
        return result;
    std::string_view path = name;
    JSValueRef current = ref->held->value;
    for (size_t start = 0;;) {
        size_t dot = path.find('.', start);
        std::string step { path.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start) };
        if (step.empty())
            return fail(ZG_ERROR_INVALID_ARGUMENT, makeString("empty step in the path \""_s, String::fromUTF8(name), '"'));
        std::string walked { path.substr(0, start ? start - 1 : 0) };
        if (!JSValueIsObject(context, current)) {
            auto type = typeOf(context, current);
            if (type == ZG_TYPE_UNDEFINED || type == ZG_TYPE_NULL)
                return fail(ZG_ERROR_NOT_FOUND, makeString(String::fromUTF8(walked.c_str()), " is "_s, String::fromLatin1(typeName(type))));
            return fail(ZG_ERROR_TYPE, makeString(walked.empty() ? "the reference"_s : String::fromUTF8(walked.c_str()), " is "_s, String::fromLatin1(typeName(type)), ", not an object"_s));
        }
        JSObjectRef object = JSValueToObject(context, current, nullptr);
        if (dot == std::string_view::npos) {
            holder = object;
            last = WTF::move(step);
            return ZG_OK;
        }
        JSStringHolder key(step);
        JSValueRef exception = nullptr;
        current = JSObjectGetProperty(context, object, key.string, &exception);
        if (exception)
            return fail(ZG_ERROR_JS_EXCEPTION, String::fromUTF8(toUTF8(context, exception).c_str()));
        start = dot + 1;
    }
}

// The value at `name`.
static zg_result getValue(zg_ref* object, const char* name, JSContextRef& context, JSValueRef& value)
{
    JSObjectRef holder;
    std::string last;
    if (auto result = resolvePath(object, name, context, holder, last); result != ZG_OK)
        return result;
    JSStringHolder key(last);
    JSValueRef exception = nullptr;
    value = JSObjectGetProperty(context, holder, key.string, &exception);
    if (exception)
        return fail(ZG_ERROR_JS_EXCEPTION, String::fromUTF8(toUTF8(context, exception).c_str()));
    return ZG_OK;
}

static char* copyString(const std::string& text, size_t* length)
{
    auto* copy = static_cast<char*>(malloc(text.size() + 1));
    memcpy(copy, text.data(), text.size());
    copy[text.size()] = 0;
    if (length)
        *length = text.size();
    return copy;
}

static zg_result expectType(JSContextRef context, JSValueRef value, zg_value_type expected)
{
    auto type = typeOf(context, value);
    if (type == expected)
        return ZG_OK;
    return fail(ZG_ERROR_TYPE, makeString("expected "_s, String::fromLatin1(typeName(expected)), ", got "_s, String::fromLatin1(typeName(type))));
}

// Typed reads of a value: strict for booleans, numbers and strings.
static zg_result readBool(JSContextRef context, JSValueRef value, int* out)
{
    if (auto result = expectType(context, value, ZG_TYPE_BOOLEAN); result != ZG_OK)
        return result;
    *out = JSValueToBoolean(context, value);
    return ZG_OK;
}

static zg_result readNumber(JSContextRef context, JSValueRef value, double* out)
{
    if (auto result = expectType(context, value, ZG_TYPE_NUMBER); result != ZG_OK)
        return result;
    *out = JSValueToNumber(context, value, nullptr);
    return ZG_OK;
}

static zg_result readString(JSContextRef context, JSValueRef value, char** out, size_t* length)
{
    if (auto result = expectType(context, value, ZG_TYPE_STRING); result != ZG_OK)
        return result;
    *out = copyString(toUTF8(context, value), length);
    return ZG_OK;
}

static zg_result readJSON(JSContextRef context, JSValueRef value, char** out, size_t* length)
{
    *out = copyString(toJSON(context, value), length);
    return ZG_OK;
}

zg_ref* zg_view_global(zg_view* view)
{
    clearError();
    if (!checkedView(view))
        return nullptr;
    RefPtr frame = view->view->page().localMainFrame();
    if (!frame) {
        fail(ZG_ERROR_NOT_FOUND, "no document"_s);
        return nullptr;
    }
    auto context = globalContext(*frame);
    return new zg_ref { std::make_shared<HeldValue>(context, JSContextGetGlobalObject(context)) };
}

zg_result zg_ref_get(zg_ref* object, const char* name, zg_ref** out)
{
    clearError();
    if (!out)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no output"_s);
    *out = nullptr;
    JSContextRef context;
    JSValueRef value;
    if (auto result = getValue(object, name, context, value); result != ZG_OK)
        return result;
    *out = new zg_ref { std::make_shared<HeldValue>(context, value) };
    return ZG_OK;
}

// zg_ref_get_<type>: the value at `name`, read by `read` (templates need C++
// linkage inside this block).
extern "C++" {
template<typename Read, typename... Out>
static zg_result getTyped(zg_ref* object, const char* name, Read read, Out*... out)
{
    clearError();
    if (((!out) || ...))
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no output"_s);
    JSContextRef context;
    JSValueRef value;
    if (auto result = getValue(object, name, context, value); result != ZG_OK)
        return result;
    return read(context, value, out...);
}
}

zg_result zg_ref_get_bool(zg_ref* object, const char* name, int* out)
{
    return getTyped(object, name, readBool, out);
}

zg_result zg_ref_get_number(zg_ref* object, const char* name, double* out)
{
    return getTyped(object, name, readNumber, out);
}

zg_result zg_ref_get_string(zg_ref* object, const char* name, char** out, size_t* length)
{
    clearError();
    if (!out)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no output"_s);
    *out = nullptr;
    JSContextRef context;
    JSValueRef value;
    if (auto result = getValue(object, name, context, value); result != ZG_OK)
        return result;
    return readString(context, value, out, length);
}

zg_result zg_ref_get_json(zg_ref* object, const char* name, char** out, size_t* length)
{
    clearError();
    if (!out)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no output"_s);
    *out = nullptr;
    JSContextRef context;
    JSValueRef value;
    if (auto result = getValue(object, name, context, value); result != ZG_OK)
        return result;
    return readJSON(context, value, out, length);
}

zg_result zg_ref_set(zg_ref* object, const char* name, const zg_value* value)
{
    clearError();
    JSContextRef context;
    JSObjectRef holder;
    std::string last;
    if (auto result = resolvePath(object, name, context, holder, last); result != ZG_OK)
        return result;
    JSValueRef exception = nullptr;
    JSValueRef converted = valueToJS(context, const_cast<zg_value*>(value), &exception);
    if (exception)
        return fail(ZG_ERROR_INVALID_ARGUMENT, makeString("value: "_s, String::fromUTF8(toUTF8(context, exception).c_str())));
    JSStringHolder key(last);
    JSObjectSetProperty(context, holder, key.string, converted, kJSPropertyAttributeNone, &exception);
    if (exception)
        return fail(ZG_ERROR_JS_EXCEPTION, String::fromUTF8(toUTF8(context, exception).c_str()));
    return ZG_OK;
}

zg_result zg_ref_call_method(zg_ref* object, const char* name, size_t argc, zg_value* const* argv, zg_ref** result)
{
    clearError();
    if (result)
        *result = nullptr;
    JSContextRef context;
    JSObjectRef holder;
    std::string last;
    if (auto status = resolvePath(object, name, context, holder, last); status != ZG_OK)
        return status;
    JSStringHolder key(last);
    JSValueRef exception = nullptr;
    JSValueRef method = JSObjectGetProperty(context, holder, key.string, &exception);
    if (exception)
        return fail(ZG_ERROR_JS_EXCEPTION, String::fromUTF8(toUTF8(context, exception).c_str()));
    JSObjectRef function = JSValueIsObject(context, method) ? JSValueToObject(context, method, nullptr) : nullptr;
    if (!function || !JSObjectIsFunction(context, function)) {
        auto type = typeOf(context, method);
        if (type == ZG_TYPE_UNDEFINED)
            return fail(ZG_ERROR_NOT_FOUND, makeString(String::fromUTF8(name), " is undefined"_s));
        return fail(ZG_ERROR_TYPE, makeString(String::fromUTF8(name), " is "_s, String::fromLatin1(typeName(type)), ", not a function"_s));
    }
    Vector<JSValueRef, 8> arguments;
    for (size_t i = 0; i < argc; ++i) {
        arguments.append(valueToJS(context, argv ? argv[i] : nullptr, &exception));
        if (exception)
            return fail(ZG_ERROR_INVALID_ARGUMENT, makeString("argument "_s, i, ": "_s, String::fromUTF8(toUTF8(context, exception).c_str())));
    }
    JSValueRef value = JSObjectCallAsFunction(context, function, holder, arguments.size(), arguments.span().data(), &exception);
    if (exception)
        return fail(ZG_ERROR_JS_EXCEPTION, String::fromUTF8(toUTF8(context, exception).c_str()));
    if (result)
        *result = new zg_ref { std::make_shared<HeldValue>(context, value) };
    return ZG_OK;
}

// zg_ref_to_<type>: the referenced value itself.
extern "C++" {
template<typename Read, typename... Out>
static zg_result toTyped(const zg_ref* ref, Read read, Out*... out)
{
    clearError();
    if (((!out) || ...))
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no output"_s);
    JSContextRef context;
    if (auto result = referenceContext(ref, context); result != ZG_OK)
        return result;
    return read(context, ref->held->value, out...);
}
}

zg_result zg_ref_to_bool(const zg_ref* ref, int* out)
{
    return toTyped(ref, readBool, out);
}

zg_result zg_ref_to_number(const zg_ref* ref, double* out)
{
    return toTyped(ref, readNumber, out);
}

zg_result zg_ref_to_string(const zg_ref* ref, char** out, size_t* length)
{
    if (out)
        *out = nullptr;
    clearError();
    if (!out)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no output"_s);
    JSContextRef context;
    if (auto result = referenceContext(ref, context); result != ZG_OK)
        return result;
    return readString(context, ref->held->value, out, length);
}

zg_result zg_ref_to_json(const zg_ref* ref, char** out, size_t* length)
{
    if (out)
        *out = nullptr;
    clearError();
    if (!out)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no output"_s);
    JSContextRef context;
    if (auto result = referenceContext(ref, context); result != ZG_OK)
        return result;
    return readJSON(context, ref->held->value, out, length);
}

void zg_free(void* pointer)
{
    free(pointer);
}

// ---- Classes ----

zg_class* zg_view_define_class(zg_view* view, const char* path, zg_bind_fn constructor, zg_finalize_fn finalize, void* user)
{
    clearError();
    if (!checkedView(view))
        return nullptr;
    if (!validPath(path)) {
        fail(ZG_ERROR_INVALID_ARGUMENT, "a class needs a dotted path"_s);
        return nullptr;
    }
    auto cls = std::make_shared<zg_class>();
    cls->core = std::make_shared<ClassCore>();
    cls->core->view = view;
    std::string_view name = path;
    cls->core->name = name.substr(name.rfind('.') == std::string_view::npos ? 0 : name.rfind('.') + 1);
    cls->core->constructor = constructor;
    cls->core->finalize = finalize;
    cls->core->user = user;
    cls->path = path;
    view->classes.push_back(cls);
    if (auto* frame = trustedFrame(*view))
        installClass(globalContext(*frame), cls);
    return cls.get();
}

static zg_result addMember(zg_class* cls, ClassMember&& member)
{
    clearError();
    if (!cls || member.name.empty())
        return fail(ZG_ERROR_INVALID_ARGUMENT, "a class member needs a class and a name"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    if (!cls->core->view)
        return fail(ZG_ERROR_NOT_FOUND, "the view of this class was destroyed"_s);
    cls->members.push_back(WTF::move(member));
    // Into the current page's prototype too.
    if (auto* frame = trustedFrame(*cls->core->view)) {
        auto* globalObject = toJS(globalContext(*frame));
        JSC::JSLockHolder lock(globalObject);
        if (auto* realm = realmFor(*cls, globalObject))
            defineMember(globalObject, realm->prototype.get(), cls->core, cls->members.back());
    }
    return ZG_OK;
}

zg_result zg_class_add_method(zg_class* cls, const char* name, zg_bind_fn function, void* user)
{
    if (!function)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "a method needs a function"_s);
    return addMember(cls, { name ? name : "", function, nullptr, nullptr, user });
}

zg_result zg_class_add_property(zg_class* cls, const char* name, zg_bind_fn getter, zg_bind_fn setter, void* user)
{
    if (!getter && !setter)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "a property needs a getter or a setter"_s);
    return addMember(cls, { name ? name : "", nullptr, getter, setter, user });
}

zg_result zg_call_set_native(zg_call* call, void* native)
{
    clearError();
    if (!call || !call->constructing || !call->instance)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "only a class's constructor sets the native pointer"_s);
    if (call->instance->native())
        return fail(ZG_ERROR_ALREADY_EXISTS, "the native pointer is already set"_s);
    call->instance->setNative(native);
    return ZG_OK;
}

void* zg_call_native(zg_call* call)
{
    return call && call->instance ? call->instance->native() : nullptr;
}

zg_value* zg_value_new_instance(zg_call* call, zg_class* cls, void* native)
{
    if (!call || !cls)
        return nullptr;
    auto* value = callValue(call);
    value->kind = zg_value::Kind::Instance;
    value->type = ZG_TYPE_OBJECT;
    value->instanceClass = cls;
    value->native = native;
    return value;
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
    if (value->kind == zg_value::Kind::Ref)
        value->text = toJSON(value->held->context, value->held->value);
    else if (value->kind != zg_value::Kind::JSON) {
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
        if (value->kind == zg_value::Kind::Plain)
            value->kind = zg_value::Kind::JSON;
    }
    if (length)
        *length = value->text.size();
    return value->text.c_str();
}

void zg_value_release(zg_value* value)
{
    // Values from zg_view_evaluate, zg_ref_call and those made without a call;
    // those made for a call belong to the call.
    delete value;
}

} // extern "C"
