/* The binding features of API 0.2, from C:
 *   - page scripts (zg_view_add_script): before the page's own scripts, again in
 *     every new document, errors to js_error;
 *   - binary values: ArrayBuffer, typed arrays (with an offset) and DataView read
 *     and written in place, Uint8Arrays returned copied and in place, Promises
 *     resolved with bytes;
 *   - references: a JavaScript function called later with every kind of argument,
 *     an object handed back as itself, and neither leaving its page;
 *   - classes: constructor, methods, accessors, instanceof, toStringTag, extends,
 *     instances made by the host, wrong `this`, and every native pointer
 *     finalized once, by the collector or when the view goes.
 *
 * Usage: bindings_test
 */

#include "zerog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...) do {                      \
        int ok_ = (condition);                          \
        printf("%s ", ok_ ? "ok  " : "FAIL");           \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
        if (!ok_)                                       \
            ++failures;                                 \
    } while (0)

static zg_runtime *runtime;
static zg_view *view;

static const char *evaluate(const char *script)
{
    static char text[1024];
    zg_value *value = NULL;
    size_t length;
    const char *json;
    if (zg_view_evaluate(view, script, strlen(script), &value) != ZG_OK) {
        snprintf(text, sizeof text, "<error: %s>", zg_last_error());
        return text;
    }
    json = zg_value_to_json(value, &length);
    snprintf(text, sizeof text, "%.*s", (int)length, json ? json : "(null)");
    zg_value_release(value);
    return text;
}

static int isTrue(const char *script)
{
    return !strcmp(evaluate(script), "true");
}

static char lastError[256];

static void onJSError(zg_view *v, const zg_js_error *err, void *user)
{
    (void)v;
    (void)user;
    snprintf(lastError, sizeof lastError, "%.*s", (int)err->message_len, err->message);
}

/* ---- Bytes ---- */

static zg_value *sum(zg_call *call, void *user)
{
    size_t length, i;
    unsigned total = 0;
    const unsigned char *bytes = zg_call_arg_bytes(call, 0, &length);
    (void)user;
    if (!bytes)
        return zg_value_null(call);
    for (i = 0; i < length; ++i)
        total += bytes[i];
    return zg_value_number(call, total * 1000.0 + length);
}

static zg_value *fill(zg_call *call, void *user)
{
    size_t length;
    void *bytes = zg_call_arg_bytes(call, 0, &length);
    (void)user;
    if (bytes)
        memset(bytes, 0xAB, length);
    return NULL;
}

static zg_value *argType(zg_call *call, void *user)
{
    (void)user;
    return zg_value_number(call, zg_call_arg_type(call, 0));
}

static zg_value *copied(zg_call *call, void *user)
{
    (void)user;
    return zg_value_bytes(call, "hello", 5);
}

static unsigned char externalData[4] = { 9, 8, 7, 6 };
static int externalReleased;

static void releaseExternal(void *user)
{
    (void)user;
    ++externalReleased;
}

static zg_value *inPlace(zg_call *call, void *user)
{
    (void)user;
    return zg_value_bytes_external(call, externalData, sizeof externalData, releaseExternal, NULL);
}

static zg_value *bytesLater(zg_call *call, void *user)
{
    zg_promise *promise;
    zg_value *value = zg_call_promise(call, &promise);
    (void)user;
    zg_promise_resolve_bytes(promise, "\x01\x02\x03", 3);
    return value;
}

/* ---- References ---- */

static zg_ref *keptFunction;
static zg_ref *keptObject;

static zg_value *keep(zg_call *call, void *user)
{
    (void)user;
    zg_ref_release(keptFunction);
    keptFunction = zg_call_arg_ref(call, 0);
    return NULL;
}

static zg_value *keepObject(zg_call *call, void *user)
{
    (void)user;
    zg_ref_release(keptObject);
    keptObject = zg_call_arg_ref(call, 0);
    return NULL;
}

static zg_value *giveBack(zg_call *call, void *user)
{
    (void)user;
    return zg_value_ref(call, keptObject);
}

/* Calls the kept function from inside a binding. */
static zg_value *callKept(zg_call *call, void *user)
{
    zg_value *argument = zg_value_number(NULL, 41);
    zg_value *result = NULL;
    (void)user;
    zg_ref_call(keptFunction, 1, &argument, &result);
    zg_value_release(argument);
    if (!result)
        return zg_value_null(call);
    {
        size_t length;
        const char *json = zg_value_to_json(result, &length);
        zg_value *returned = zg_value_json(call, json, length);
        zg_value_release(result);
        return returned;
    }
}

/* ---- Classes ---- */

typedef struct Counter {
    double value;
} Counter;

static int countersMade, countersFinalized;
static zg_class *counterClass, *handleClass;

static void finalizeCounter(void *native, void *user)
{
    (void)user;
    free(native);
    ++countersFinalized;
}

static Counter *newCounter(double start)
{
    Counter *counter = malloc(sizeof *counter);
    counter->value = start;
    ++countersMade;
    return counter;
}

static zg_value *constructCounter(zg_call *call, void *user)
{
    double start = zg_call_argc(call) ? zg_call_arg_number(call, 0) : 0;
    (void)user;
    if (start < 0)
        return zg_call_throw(call, "a counter starts at 0 or more");
    zg_call_set_native(call, newCounter(start));
    return NULL;
}

static zg_value *increment(zg_call *call, void *user)
{
    Counter *counter = zg_call_native(call);
    (void)user;
    counter->value += 1;
    return zg_value_number(call, counter->value);
}

static zg_value *getValue(zg_call *call, void *user)
{
    Counter *counter = zg_call_native(call);
    (void)user;
    return zg_value_number(call, counter->value);
}

static zg_value *setValue(zg_call *call, void *user)
{
    Counter *counter = zg_call_native(call);
    (void)user;
    counter->value = zg_call_arg_number(call, 0);
    return NULL;
}

static zg_value *getKind(zg_call *call, void *user)
{
    (void)user;
    return zg_value_string(call, "counter");
}

static zg_value *makeCounter(zg_call *call, void *user)
{
    (void)user;
    return zg_value_new_instance(call, counterClass, newCounter(zg_call_arg_number(call, 0)));
}

static zg_value *openHandle(zg_call *call, void *user)
{
    (void)user;
    return zg_value_new_instance(call, handleClass, newCounter(-1));
}

static void collect(void)
{
    int i;
    for (i = 0; i < 5; ++i) {
        zg_runtime_collect_garbage(runtime);
        zg_runtime_update(runtime);
    }
}

int main(void)
{
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context_config contextConfig = { sizeof contextConfig };
    zg_view_config viewConfig = { sizeof viewConfig };
    zg_view_callbacks callbacks = { sizeof callbacks };
    zg_context *context;
    static const char page[] =
        "<!DOCTYPE html><title>bindings</title>"
        "<script>window.pageSaw = window.fromScript; window.pageSawClass = typeof App.Counter;</script>";
    static const char initScript[] = "window.fromScript = (window.fromScript || 0) + 1; window.scriptSawBindings = typeof App.sum;";
    static const char badScript[] = "throw new Error('init boom')";
    zg_value *unused;
    zg_result result;

    setvbuf(stdout, NULL, _IONBF, 0);
    runtime = zg_runtime_create(&runtimeConfig);
    contextConfig.ephemeral = 1;
    context = zg_context_create(runtime, &contextConfig);
    /* app:// with files is trusted: its pages get the bindings. */
    zg_context_add_file(context, "app", "/index.html", "", 0, "text/html", NULL, NULL);
    view = zg_view_create(context, &viewConfig);
    callbacks.js_error = onJSError;
    zg_view_set_callbacks(view, &callbacks);

    zg_view_bind(view, "App.sum", sum, NULL);
    zg_view_bind(view, "App.fill", fill, NULL);
    zg_view_bind(view, "App.argType", argType, NULL);
    zg_view_bind(view, "App.copied", copied, NULL);
    zg_view_bind(view, "App.inPlace", inPlace, NULL);
    zg_view_bind(view, "App.bytesLater", bytesLater, NULL);
    zg_view_bind(view, "App.keep", keep, NULL);
    zg_view_bind(view, "App.keepObject", keepObject, NULL);
    zg_view_bind(view, "App.giveBack", giveBack, NULL);
    zg_view_bind(view, "App.callKept", callKept, NULL);
    zg_view_bind(view, "App.makeCounter", makeCounter, NULL);
    zg_view_bind(view, "App.openHandle", openHandle, NULL);
    counterClass = zg_view_define_class(view, "App.Counter", constructCounter, finalizeCounter, NULL);
    CHECK(counterClass != NULL, "define App.Counter");
    zg_class_add_method(counterClass, "inc", increment, NULL);
    zg_class_add_property(counterClass, "value", getValue, setValue, NULL);
    zg_class_add_property(counterClass, "kind", getKind, NULL, NULL);
    handleClass = zg_view_define_class(view, "App.Handle", NULL, finalizeCounter, NULL);
    zg_view_add_script(view, initScript, sizeof initScript - 1);
    zg_view_add_script(view, badScript, sizeof badScript - 1);
    zg_view_load_html(view, page, sizeof page - 1, "app://localhost/");

    /* ---- Page scripts ---- */
    CHECK(isTrue("pageSaw === 1"), "a page script runs before the page's own scripts: %s", evaluate("pageSaw"));
    CHECK(isTrue("scriptSawBindings === 'function' && pageSawClass === 'function'"), "after the bindings and classes: %s", evaluate("[scriptSawBindings, pageSawClass]"));
    CHECK(strstr(lastError, "init boom") != NULL, "its errors reach js_error: %s", lastError);

    /* ---- Bytes ---- */
    CHECK(!strcmp(evaluate("App.sum(new Uint8Array([1, 2, 3]))"), "6003"), "a Uint8Array read in place: %s", evaluate("App.sum(new Uint8Array([1, 2, 3]))"));
    CHECK(!strcmp(evaluate("App.sum(new Uint8Array([1, 2, 3, 4, 5]).buffer)"), "15005"), "an ArrayBuffer");
    CHECK(!strcmp(evaluate("App.sum(new Uint8Array([1, 2, 3, 4, 5]).subarray(2, 4))"), "7002"), "a view with an offset: %s", evaluate("App.sum(new Uint8Array([1, 2, 3, 4, 5]).subarray(2, 4))"));
    CHECK(!strcmp(evaluate("App.sum(new DataView(new Uint8Array([7, 7]).buffer, 1))"), "7001"), "a DataView: %s", evaluate("App.sum(new DataView(new Uint8Array([7, 7]).buffer, 1))"));
    CHECK(!strcmp(evaluate("App.sum(new Uint16Array([256]))"), "1002"), "other typed arrays as bytes");
    CHECK(!strcmp(evaluate("App.sum(new Uint8Array(0))"), "0"), "an empty buffer is still bytes");
    CHECK(!strcmp(evaluate("App.sum([1, 2])"), "null") && !strcmp(evaluate("App.sum('ab')"), "null"), "other values are not bytes");
    CHECK(isTrue("(() => { const b = new Uint8Array(3); App.fill(b); return b.every(x => x === 0xAB); })()"), "a binding fills a page's buffer");
    {
        char script[64];
        snprintf(script, sizeof script, "App.argType(new Float32Array(1)) === %d", ZG_TYPE_BYTES);
        CHECK(isTrue(script), "typed arrays are ZG_TYPE_BYTES");
    }
    CHECK(isTrue("(() => { const b = App.copied(); return b instanceof Uint8Array && String.fromCharCode(...b) === 'hello'; })()"), "zg_value_bytes gives a Uint8Array");
    CHECK(isTrue("(() => { window.inPlace = App.inPlace(); return inPlace.join() === '9,8,7,6'; })()"), "zg_value_bytes_external is used in place");
    CHECK(isTrue("(() => { inPlace[0] = 1; return true; })()") && externalData[0] == 1, "and is the application's memory");
    evaluate("window.inPlace = null; 0");
    collect();
    CHECK(externalReleased == 1, "released once the buffer is collected (%d)", externalReleased);
    unused = zg_value_bytes_external(NULL, externalData, 4, releaseExternal, NULL);
    zg_value_release(unused);
    CHECK(externalReleased == 2, "or when the value is released unused (%d)", externalReleased);
    evaluate("App.bytesLater().then(b => window.later = b instanceof Uint8Array ? b.join() : 'no'); 0");
    zg_runtime_update(runtime);
    CHECK(isTrue("later === '1,2,3'"), "a Promise resolved with bytes: %s", evaluate("window.later"));

    /* ---- References ---- */
    evaluate("App.keep((...args) => { window.got = args; return { sum: args.length }; }); 0");
    {
        zg_value *argv[5];
        zg_value *returned = NULL;
        size_t length;
        argv[0] = zg_value_number(NULL, 1.5);
        argv[1] = zg_value_string(NULL, "two");
        argv[2] = zg_value_json(NULL, "{\"three\":[3]}", 13);
        argv[3] = zg_value_bytes(NULL, "\x04", 1);
        argv[4] = zg_value_int64(NULL, (int64_t)1 << 60);
        CHECK(zg_ref_type(keptFunction) == ZG_TYPE_FUNCTION, "a kept function");
        result = zg_ref_call(keptFunction, 5, argv, &returned);
        CHECK(result == ZG_OK && !strcmp(zg_value_to_json(returned, &length), "{\"sum\":5}"), "called later, its result: %s", returned ? zg_value_to_json(returned, &length) : zg_last_error());
        CHECK(isTrue("got[0] === 1.5 && got[1] === 'two' && got[2].three[0] === 3 && got[3] instanceof Uint8Array && got[3][0] === 4 && got[4] === 2n ** 60n"),
            "with every kind of argument");
        zg_value_release(returned);
        result = zg_ref_call(keptFunction, 5, argv, NULL);
        CHECK(result == ZG_OK, "the arguments are not consumed");
        for (length = 0; length < 5; ++length)
            zg_value_release(argv[length]);
    }
    CHECK(!strcmp(evaluate("App.callKept()"), "{\"sum\":1}") && isTrue("got[0] === 41"), "and from inside a binding");
    evaluate("App.keep(() => { throw new Error('thrown back'); }); 0");
    result = zg_ref_call(keptFunction, 0, NULL, NULL);
    CHECK(result == ZG_ERROR_JS_EXCEPTION && strstr(zg_last_error(), "thrown back"), "its exception: %s", zg_last_error());
    CHECK(isTrue("(() => { const o = { n: 1 }; App.keepObject(o); return App.giveBack() === o; })()"), "an object handed back is itself");

    /* ---- Classes ---- */
    CHECK(isTrue("(() => { const c = new App.Counter(5); c.inc(); return c.value === 6 && c instanceof App.Counter; })()"), "constructor, method and getter: %s", evaluate("new App.Counter(5).value"));
    CHECK(isTrue("(() => { const c = new App.Counter(); c.value = 10; c.inc(); return c.value; })() === 11"), "a setter");
    CHECK(isTrue("(() => { const c = new App.Counter(); c.kind = 'x'; return c.kind === 'counter'; })()"), "a read-only property");
    CHECK(isTrue("Object.prototype.toString.call(new App.Counter()) === '[object Counter]' && App.Counter.name === 'Counter'"), "Symbol.toStringTag and name");
    CHECK(isTrue("(() => { try { App.Counter(1); return false; } catch (e) { return e instanceof TypeError; } })()"), "calling without new throws a TypeError");
    CHECK(isTrue("(() => { try { App.Counter.prototype.inc.call({}); return false; } catch (e) { return e instanceof TypeError; } })()"), "a method on another object throws a TypeError");
    CHECK(isTrue("(() => { try { new App.Counter(-1); return false; } catch (e) { return /starts at 0/.test(e.message); } })()"), "a constructor that throws");
    CHECK(isTrue("(() => { class Twice extends App.Counter { twice() { this.inc(); return this.inc(); } }; const t = new Twice(1);"
                 "  return t.twice() === 3 && t instanceof Twice && t instanceof App.Counter; })()"), "extends: %s", evaluate("(() => { try { class T extends App.Counter {}; return new T(1).value; } catch (e) { return String(e); } })()"));
    CHECK(isTrue("(() => { const c = App.makeCounter(7); return c instanceof App.Counter && c.value === 7; })()"), "an instance made by the host");
    CHECK(isTrue("(() => { try { new App.Handle(); return false; } catch (e) { return e instanceof TypeError; } })() && App.openHandle() instanceof App.Handle"),
        "a class without constructor: instances only from the host");
    evaluate("for (let i = 0; i < 1000; ++i) new App.Counter(i); 0");
    collect();
    /* None of them is reachable any more. */
    CHECK(countersFinalized == countersMade, "the collector finalizes instances: %d of %d", countersFinalized, countersMade);
    evaluate("window.keepAlive = [new App.Counter(1), App.makeCounter(2)]; 0");

    /* A new document: the page script runs again, references stay with the old one. */
    zg_view_load_html(view, page, sizeof page - 1, "app://localhost/");
    CHECK(isTrue("pageSaw === 1 && typeof App.Counter === 'function'"), "a new document gets the scripts and classes again: %s",
        evaluate("[window.pageSaw, window.fromScript, typeof App, typeof window.keepAlive, document.title]"));
    CHECK(isTrue("(() => { try { App.giveBack(); return false; } catch (e) { return /page it came from/.test(e.message); } })()"), "a reference does not go into another page");
    evaluate("App.keep(() => 1); 0");
    {
        zg_ref *old = keptFunction;
        keptFunction = NULL;
        evaluate("0");
        zg_view_load_html(view, page, sizeof page - 1, "app://localhost/");
        result = zg_ref_call(old, 0, NULL, NULL);
        CHECK(result == ZG_ERROR_NOT_FOUND, "a function whose page is gone is not called: %s", zg_last_error());
        zg_ref_release(old);
    }
    zg_ref_release(keptObject);
    keptObject = NULL;

    zg_view_destroy(view);
    collect();
    CHECK(countersFinalized == countersMade, "every native pointer finalized once, the rest when the view went: %d of %d", countersFinalized, countersMade);

    zg_runtime_destroy(runtime);
    printf(failures ? "FAILED\n" : "PASS\n");
    return failures ? 1 : 0;
}
