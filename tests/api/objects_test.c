/* A page's objects from native code (API 0.4), from C:
 *   - the global object, properties and dotted paths read and set, methods
 *     called with `this` their object, DOM elements changed as JavaScript would;
 *   - typed reads that are strict (ZG_ERROR_TYPE, with the types in the message)
 *     and the JSON read that converts anything;
 *   - references as results and as values set, bytes set as Uint8Arrays;
 *   - errors as results: a missing step on a path, a value that is not an object
 *     or not a function, a method that throws, a reference whose page is gone,
 *     bad arguments.
 *
 * Usage: objects_test
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

static const char page[] =
    "<!DOCTYPE html><html><head><title>Objects</title></head><body>"
    "<h1 id='title'>Before</h1>"
    "<script>"
    "  window.app = { count: 3, ready: true, name: 'ZeroG', items: [10, 20, 30],"
    "    nested: { deep: { value: 'found' } },"
    "    add(a, b) { return a + b; },"
    "    self() { return this === window.app; },"
    "    fail() { throw new Error('on purpose'); } };"
    "  Object.defineProperty(window.app, 'trap', { get() { throw new Error('getter threw'); } });"
    "</script></body></html>";

int main(void)
{
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context_config contextConfig = { sizeof contextConfig };
    zg_view_config viewConfig = { sizeof viewConfig };
    zg_runtime *runtime;
    zg_context *context;
    zg_ref *window, *title = NULL, *ref = NULL;
    char *text = NULL;
    size_t length = 0;
    double number = 0;
    int flag = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    runtime = zg_runtime_create(&runtimeConfig);
    contextConfig.ephemeral = 1;
    context = zg_context_create(runtime, &contextConfig);
    viewConfig.width = 320;
    viewConfig.height = 200;
    view = zg_view_create(context, &viewConfig);
    zg_view_load_html(view, page, sizeof page - 1, NULL);

    window = zg_view_global(view);
    CHECK(window && zg_ref_type(window) == ZG_TYPE_OBJECT, "the global object");

    /* The example of include/zerog.h: document.getElementById("title").textContent = "Hello". */
    {
        zg_value *id = zg_value_string(NULL, "title");
        zg_value *hello = zg_value_string(NULL, "Hello");
        CHECK(zg_ref_call_method(window, "document.getElementById", 1, &id, &title) == ZG_OK && title, "document.getElementById(\"title\") returns the element");
        CHECK(zg_ref_set(title, "textContent", hello) == ZG_OK && !strcmp(evaluate("document.getElementById('title').textContent"), "\"Hello\""), "its textContent set from C");
        zg_value_release(id);
        zg_value_release(hello);
    }
    {
        zg_value *red = zg_value_string(NULL, "rgb(255, 0, 0)");
        CHECK(zg_ref_set(title, "style.color", red) == ZG_OK && zg_ref_get_string(title, "style.color", &text, &length) == ZG_OK && !strcmp(text, "rgb(255, 0, 0)") && length == 14,
            "a dotted path into the element's style: %s", text ? text : zg_last_error());
        zg_free(text);
        text = NULL;
        zg_value_release(red);
    }

    /* Typed reads. */
    CHECK(zg_ref_get_string(window, "document.title", &text, &length) == ZG_OK && !strcmp(text, "Objects"), "document.title: %s", text ? text : zg_last_error());
    zg_free(text);
    text = NULL;
    CHECK(zg_ref_get_number(window, "app.count", &number) == ZG_OK && number == 3, "a number: %g", number);
    CHECK(zg_ref_get_bool(window, "app.ready", &flag) == ZG_OK && flag == 1, "a boolean");
    CHECK(zg_ref_get_number(window, "app.items.1", &number) == ZG_OK && number == 20, "an array element by index on the path: %g", number);
    CHECK(zg_ref_get_string(window, "app.nested.deep.value", &text, &length) == ZG_OK && !strcmp(text, "found"), "a deeper path");
    zg_free(text);
    text = NULL;
    CHECK(zg_ref_get_json(window, "app.items", &text, &length) == ZG_OK && !strcmp(text, "[10,20,30]"), "JSON of an array: %s", text ? text : "");
    zg_free(text);
    text = NULL;

    /* Strict types. */
    CHECK(zg_ref_get_number(window, "app.name", &number) == ZG_ERROR_TYPE && strstr(zg_last_error(), "expected a number, got a string"), "a string read as a number: %s", zg_last_error());
    CHECK(zg_ref_get_string(window, "app.count", &text, &length) == ZG_ERROR_TYPE && !text, "a number read as a string: %s", zg_last_error());
    CHECK(zg_ref_get_bool(window, "app.count", &flag) == ZG_ERROR_TYPE, "a number read as a boolean");

    /* Setting values of every kind, and references. */
    {
        zg_value *values[4];
        values[0] = zg_value_number(NULL, 42);
        values[1] = zg_value_bool(NULL, 0);
        values[2] = zg_value_json(NULL, "{\"a\":[1,2]}", 11);
        values[3] = zg_value_ref(NULL, title);
        CHECK(zg_ref_set(window, "app.count", values[0]) == ZG_OK && !strcmp(evaluate("app.count"), "42"), "a number set");
        CHECK(zg_ref_set(window, "app.ready", values[1]) == ZG_OK && !strcmp(evaluate("app.ready"), "false"), "a boolean set");
        CHECK(zg_ref_set(window, "app.data", values[2]) == ZG_OK && !strcmp(evaluate("app.data.a[1]"), "2"), "JSON set as an object");
        CHECK(zg_ref_set(window, "app.saved", values[3]) == ZG_OK && !strcmp(evaluate("app.saved === document.getElementById('title')"), "true"), "a reference set: the same element, not a copy");
        CHECK(zg_ref_set(window, "app.gone", NULL) == ZG_OK && !strcmp(evaluate("'gone' in app && app.gone === undefined"), "true"), "NULL sets undefined");
        zg_value_release(values[0]);
        zg_value_release(values[1]);
        zg_value_release(values[2]);
        zg_value_release(values[3]);
    }
    {
        const unsigned char bytes[] = { 1, 2, 3, 250 };
        zg_value *data = zg_value_bytes(NULL, bytes, sizeof bytes);
        CHECK(zg_ref_set(window, "app.bytes", data) == ZG_OK && !strcmp(evaluate("app.bytes instanceof Uint8Array && Array.from(app.bytes).join()"), "\"1,2,3,250\""), "bytes set as a Uint8Array");
        zg_value_release(data);
    }

    /* Methods: `this`, arguments, results as references. */
    {
        zg_value *arguments[2];
        arguments[0] = zg_value_number(NULL, 2);
        arguments[1] = zg_value_number(NULL, 3);
        CHECK(zg_ref_call_method(window, "app.add", 2, arguments, &ref) == ZG_OK && zg_ref_to_number(ref, &number) == ZG_OK && number == 5, "a method's result: %g", number);
        zg_ref_release(ref);
        ref = NULL;
        CHECK(zg_ref_call_method(window, "app.self", 0, NULL, &ref) == ZG_OK && zg_ref_to_bool(ref, &flag) == ZG_OK && flag, "`this` is the method's object");
        zg_ref_release(ref);
        ref = NULL;
        CHECK(zg_ref_call_method(window, "app.add", 2, arguments, NULL) == ZG_OK, "a result is optional");
        zg_value_release(arguments[0]);
        zg_value_release(arguments[1]);
    }
    CHECK(zg_ref_get(window, "app.nested", &ref) == ZG_OK && zg_ref_to_json(ref, &text, &length) == ZG_OK && !strcmp(text, "{\"deep\":{\"value\":\"found\"}}"), "a property as a reference: %s", text ? text : "");
    zg_free(text);
    text = NULL;
    CHECK(zg_ref_get_string(ref, "deep.value", &text, &length) == ZG_OK && !strcmp(text, "found"), "and paths from it");
    zg_free(text);
    text = NULL;
    zg_ref_release(ref);
    ref = NULL;
    CHECK(zg_ref_get(window, "app.missing", &ref) == ZG_OK && zg_ref_type(ref) == ZG_TYPE_UNDEFINED, "a missing last step is undefined");
    zg_ref_release(ref);
    ref = NULL;

    /* Errors as results. */
    CHECK(zg_ref_get_number(window, "app.nope.value", &number) == ZG_ERROR_NOT_FOUND, "a missing step on the path: %s", zg_last_error());
    CHECK(zg_ref_get_number(window, "app.count.value", &number) == ZG_ERROR_TYPE, "a step that is not an object: %s", zg_last_error());
    CHECK(zg_ref_call_method(window, "app.count", 0, NULL, NULL) == ZG_ERROR_TYPE, "a property that is not a function: %s", zg_last_error());
    CHECK(zg_ref_call_method(window, "app.nothing", 0, NULL, NULL) == ZG_ERROR_NOT_FOUND, "a method that does not exist: %s", zg_last_error());
    CHECK(zg_ref_call_method(window, "app.fail", 0, NULL, NULL) == ZG_ERROR_JS_EXCEPTION && strstr(zg_last_error(), "on purpose"), "a method that throws: %s", zg_last_error());
    CHECK(zg_ref_get_number(window, "app.trap", &number) == ZG_ERROR_JS_EXCEPTION && strstr(zg_last_error(), "getter threw"), "a getter that throws: %s", zg_last_error());
    CHECK(zg_ref_get_number(window, "app..count", &number) == ZG_ERROR_INVALID_ARGUMENT, "an empty step: %s", zg_last_error());
    CHECK(zg_ref_get_number(window, NULL, &number) == ZG_ERROR_INVALID_ARGUMENT, "no name");
    CHECK(zg_ref_get_number(NULL, "app.count", &number) == ZG_ERROR_INVALID_ARGUMENT, "no reference");
    CHECK(zg_ref_get_number(window, "app.count", NULL) == ZG_ERROR_INVALID_ARGUMENT, "no output");

    /* A new document: references into the old one find their page gone. */
    zg_view_load_html(view, "<p>next</p>", 11, NULL);
    CHECK(zg_ref_get_number(window, "app.count", &number) == ZG_ERROR_NOT_FOUND, "the old global object, after a new document: %s", zg_last_error());
    CHECK(zg_ref_set(title, "textContent", NULL) == ZG_ERROR_NOT_FOUND, "and an element of the old document");
    zg_ref_release(window);
    window = zg_view_global(view);
    CHECK(window && zg_ref_get_string(window, "document.body.textContent", &text, &length) == ZG_OK && !strcmp(text, "next"), "the new document's global object");
    zg_free(text);

    zg_ref_release(title);
    zg_ref_release(window);
    zg_view_destroy(view);
    zg_context_destroy(context);
    zg_runtime_destroy(runtime);
    printf(failures ? "FAILED\n" : "PASS\n");
    return failures ? 1 : 0;
}
