/* Value conversions between JavaScript and the host, for every type of the C API
 * (docs/design/testing-ci-upstream.md, "Test levels": the binding level):
 *   - every JavaScript type read by a binding with each zg_call_arg_* function;
 *   - every zg_value_* returned to JavaScript;
 *   - Promises resolved with JSON values and rejected;
 *   - zg_view_evaluate results, their type and JSON.
 *
 * Usage: values_test
 */

#include "zerog.h"

#include <math.h>
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

static const char *typeName(zg_value_type type)
{
    static const char *names[] = { "undefined", "null", "boolean", "number", "bigint", "string", "array", "object", "function" };
    return (unsigned)type < sizeof names / sizeof *names ? names[type] : "?";
}

/* Numbers printed the same way on every C runtime. */
static void formatNumber(char *out, size_t size, double number)
{
    if (isnan(number))
        snprintf(out, size, "NaN");
    else if (isinf(number))
        snprintf(out, size, number > 0 ? "Infinity" : "-Infinity");
    else if (number == 0 && signbit(number))
        snprintf(out, size, "-0");
    else
        snprintf(out, size, "%.17g", number);
}

/* ---- JavaScript to the host: what each reader gives for argument 0 ---- */

static zg_value *inspect(zg_call *call, void *user)
{
    char result[2048], number[64];
    size_t stringLength, jsonLength;
    const char *string = zg_call_arg_string(call, 0, &stringLength);
    const char *json = zg_call_arg_json(call, 0, &jsonLength);
    size_t i;
    char escaped[512] = { 0 };
    (void)user;
    /* NUL and other control characters shown as \xNN. */
    for (i = 0; string && i < stringLength && strlen(escaped) < sizeof escaped - 8; ++i) {
        unsigned char c = (unsigned char)string[i];
        if (c < 0x20)
            snprintf(escaped + strlen(escaped), sizeof escaped - strlen(escaped), "\\x%02x", c);
        else
            escaped[strlen(escaped)] = (char)c;
    }
    formatNumber(number, sizeof number, zg_call_arg_number(call, 0));
    snprintf(result, sizeof result, "argc=%u type=%s bool=%d number=%s int64=%lld string=%u:%s json=%.*s",
        (unsigned)zg_call_argc(call), typeName(zg_call_arg_type(call, 0)), zg_call_arg_bool(call, 0), number,
        (long long)zg_call_arg_int64(call, 0), (unsigned)stringLength, string ? escaped : "(null)", json ? (int)jsonLength : 6, json ? json : "(null)");
    return zg_value_string(call, result);
}

/* ---- The host to JavaScript: one value of each kind ---- */

static zg_value *make(zg_call *call, void *user)
{
    size_t length;
    const char *kind = zg_call_arg_string(call, 0, &length);
    (void)user;
    if (!strcmp(kind, "undefined"))
        return NULL;
    if (!strcmp(kind, "null"))
        return zg_value_null(call);
    if (!strcmp(kind, "true"))
        return zg_value_bool(call, 1);
    if (!strcmp(kind, "false"))
        return zg_value_bool(call, 0);
    if (!strcmp(kind, "nan"))
        return zg_value_number(call, NAN);
    if (!strcmp(kind, "infinity"))
        return zg_value_number(call, -INFINITY);
    if (!strcmp(kind, "negative-zero"))
        return zg_value_number(call, -0.0);
    if (!strcmp(kind, "safe-integer"))
        return zg_value_int64(call, INT64_C(9007199254740991));
    if (!strcmp(kind, "big-integer"))
        return zg_value_int64(call, INT64_C(9007199254740992));
    if (!strcmp(kind, "minimum-integer"))
        return zg_value_int64(call, INT64_MIN);
    if (!strcmp(kind, "string"))
        return zg_value_string(call, "h\xc3\xa9llo \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x98\x80");
    if (!strcmp(kind, "string-with-nul"))
        return zg_value_string_len(call, "a\0b", 3);
    if (!strcmp(kind, "json"))
        return zg_value_json(call, "{\"a\":[1,{\"b\":null}],\"s\":\"\\u00e9\"}", strlen("{\"a\":[1,{\"b\":null}],\"s\":\"\\u00e9\"}"));
    if (!strcmp(kind, "json-invalid"))
        return zg_value_json(call, "{nope", 5);
    return zg_call_throw(call, "nope");
}

/* ---- Promises ---- */

static zg_value *promise(zg_call *call, void *user)
{
    size_t length;
    const char *kind = zg_call_arg_string(call, 0, &length);
    zg_promise *pending;
    zg_value *value = zg_call_promise(call, &pending);
    (void)user;
    if (!strcmp(kind, "array"))
        zg_promise_resolve(pending, "[1,\"two\",{\"three\":3}]", strlen("[1,\"two\",{\"three\":3}]"));
    else if (!strcmp(kind, "empty"))
        zg_promise_resolve(pending, NULL, 0);
    else if (!strcmp(kind, "invalid"))
        zg_promise_resolve(pending, "{oops", 5);
    else
        zg_promise_reject(pending, "rejected by the host");
    return value;
}

static zg_runtime *runtime;
static zg_view *view;

/* Evaluates `script` and returns its JSON (a static buffer). */
static const char *evaluate(const char *script)
{
    static char text[4096];
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

/* Runs pending work until `expression` is true in the page (up to 2 s). */
static int waitFor(const char *expression)
{
    int i;
    for (i = 0; i < 400; ++i) {
        if (!strcmp(evaluate(expression), "true"))
            return 1;
        zg_runtime_update(runtime);
    }
    return 0;
}

int main(void)
{
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context_config contextConfig = { sizeof contextConfig };
    zg_view_config viewConfig = { sizeof viewConfig };
    static const char page[] = "<!DOCTYPE html><title>values</title>";
    size_t i;

    /* JavaScript expression, and what inspect() reads from it. */
    static const struct { const char *expression, *expected; } arguments[] = {
        { "undefined", "argc=1 type=undefined bool=0 number=NaN int64=0 string=9:undefined json=null" },
        { "null", "argc=1 type=null bool=0 number=0 int64=0 string=4:null json=null" },
        { "true", "argc=1 type=boolean bool=1 number=1 int64=1 string=4:true json=true" },
        { "false", "argc=1 type=boolean bool=0 number=0 int64=0 string=5:false json=false" },
        { "0", "argc=1 type=number bool=0 number=0 int64=0 string=1:0 json=0" },
        { "-0", "argc=1 type=number bool=0 number=-0 int64=0 string=1:0 json=0" },
        { "1.5", "argc=1 type=number bool=1 number=1.5 int64=1 string=3:1.5 json=1.5" },
        { "NaN", "argc=1 type=number bool=0 number=NaN int64=0 string=3:NaN json=null" },
        { "-Infinity", "argc=1 type=number bool=1 number=-Infinity int64=-9223372036854775808 string=9:-Infinity json=null" },
        { "2 ** 53", "argc=1 type=number bool=1 number=9007199254740992 int64=9007199254740992 string=16:9007199254740992 json=9007199254740992" },
        { "1e300", "argc=1 type=number bool=1 number=1.0000000000000001e+300 int64=9223372036854775807 string=6:1e+300 json=1e+300" },
        { "-1e300", "argc=1 type=number bool=1 number=-1.0000000000000001e+300 int64=-9223372036854775808 string=7:-1e+300 json=-1e+300" },
        { "2n ** 53n + 1n", "argc=1 type=bigint bool=1 number=9007199254740992 int64=9007199254740993 string=16:9007199254740993 json=null" },
        { "-(2n ** 63n)", "argc=1 type=bigint bool=1 number=-9.2233720368547758e+18 int64=-9223372036854775808 string=20:-9223372036854775808 json=null" },
        { "''", "argc=1 type=string bool=0 number=0 int64=0 string=0: json=\"\"" },
        { "'h\\u00e9llo \\u65e5\\u672c \\ud83d\\ude00'", "argc=1 type=string bool=1 number=NaN int64=0 string=18:h\xc3\xa9llo \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x98\x80 json=\"h\xc3\xa9llo \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x98\x80\"" },
        { "'a\\0b'", "argc=1 type=string bool=1 number=NaN int64=0 string=3:a\\x00b json=\"a\\u0000b\"" },
        { "'  42  '", "argc=1 type=string bool=1 number=42 int64=42 string=6:  42   json=\"  42  \"" },
        { "[1, 'x', null]", "argc=1 type=array bool=1 number=NaN int64=0 string=4:1,x, json=[1,\"x\",null]" },
        { "({ a: { b: [true] } })", "argc=1 type=object bool=1 number=NaN int64=0 string=15:[object Object] json={\"a\":{\"b\":[true]}}" },
        { "(() => 1)", "argc=1 type=function bool=1 number=NaN int64=0 string=7:() => 1 json=null" },
    };

    /* Expressions on what make() returns; each must be true. */
    static const char *returns[] = {
        "App.make('undefined') === undefined",
        "App.make('null') === null",
        "App.make('true') === true && App.make('false') === false",
        "Number.isNaN(App.make('nan'))",
        "App.make('infinity') === -Infinity",
        "Object.is(App.make('negative-zero'), -0)",
        "typeof App.make('safe-integer') === 'number' && App.make('safe-integer') === 9007199254740991",
        "typeof App.make('big-integer') === 'bigint' && App.make('big-integer') === 9007199254740992n",
        "App.make('minimum-integer') === -(2n ** 63n)",
        "App.make('string') === 'h\\u00e9llo \\u65e5\\u672c \\ud83d\\ude00'",
        "App.make('string-with-nul') === 'a\\0b'",
        "JSON.stringify(App.make('json')) === '{\"a\":[1,{\"b\":null}],\"s\":\"\\u00e9\"}'",
        "(() => { try { App.make('json-invalid'); return false; } catch (e) { return e instanceof Error && /invalid JSON/.test(e.message); } })()",
        "(() => { try { App.make('throw'); return false; } catch (e) { return e instanceof Error && e.message === 'nope'; } })()",
    };

    /* zg_view_evaluate: type and JSON of each result. */
    static const struct { const char *script; zg_value_type type; const char *json; } results[] = {
        { "undefined", ZG_TYPE_UNDEFINED, "null" },
        { "null", ZG_TYPE_NULL, "null" },
        { "1 + 1", ZG_TYPE_NUMBER, "2" },
        { "'s\\u00e9'", ZG_TYPE_STRING, "\"s\xc3\xa9\"" },
        { "[1, [2]]", ZG_TYPE_ARRAY, "[1,[2]]" },
        { "({ k: 'v' })", ZG_TYPE_OBJECT, "{\"k\":\"v\"}" },
        { "true", ZG_TYPE_BOOLEAN, "true" },
    };

    setvbuf(stdout, NULL, _IONBF, 0);
    runtime = zg_runtime_create(&runtimeConfig);
    contextConfig.ephemeral = 1;
    view = zg_view_create(zg_context_create(runtime, &contextConfig), &viewConfig);
    zg_view_bind(view, "App.inspect", inspect, NULL);
    zg_view_bind(view, "App.make", make, NULL);
    zg_view_bind(view, "App.promise", promise, NULL);
    zg_view_load_html(view, page, sizeof page - 1, NULL);

    for (i = 0; i < sizeof arguments / sizeof *arguments; ++i) {
        char script[512], expected[1024];
        const char *seen;
        snprintf(script, sizeof script, "App.inspect(%s)", arguments[i].expression);
        seen = evaluate(script);
        /* evaluate() gives the JSON of the returned string: compare with that. */
        snprintf(expected, sizeof expected, "\"%s\"", arguments[i].expected);
        {
            /* The expected text has backslashes of its own; quote them as JSON does. */
            char quoted[1024];
            size_t in, out = 0;
            quoted[out++] = '"';
            for (in = 0; arguments[i].expected[in] && out < sizeof quoted - 3; ++in) {
                if (arguments[i].expected[in] == '"' || arguments[i].expected[in] == '\\')
                    quoted[out++] = '\\';
                quoted[out++] = arguments[i].expected[in];
            }
            quoted[out++] = '"';
            quoted[out] = 0;
            CHECK(!strcmp(seen, quoted), "argument %s\n         got      %s\n         expected %s", arguments[i].expression, seen, quoted);
        }
    }
    CHECK(!strcmp(evaluate("App.inspect()"), "\"argc=0 type=undefined bool=0 number=NaN int64=0 string=0:(null) json=(null)\""),
        "a missing argument: %s", evaluate("App.inspect()"));

    for (i = 0; i < sizeof returns / sizeof *returns; ++i)
        CHECK(!strcmp(evaluate(returns[i]), "true"), "returned: %s", returns[i]);

    evaluate("window.settled = {};"
        "for (const kind of ['array', 'empty', 'invalid', 'reject'])"
        "  App.promise(kind).then(v => settled[kind] = 'resolved ' + JSON.stringify(v), e => settled[kind] = 'rejected ' + e.message); 0");
    CHECK(waitFor("Object.keys(settled).length === 4"), "four Promises settled");
    CHECK(!strcmp(evaluate("settled.array"), "\"resolved [1,\\\"two\\\",{\\\"three\\\":3}]\""), "resolved with a JSON array: %s", evaluate("settled.array"));
    CHECK(!strcmp(evaluate("settled.empty"), "\"resolved null\""), "resolved without a value: %s", evaluate("settled.empty"));
    CHECK(strstr(evaluate("settled.invalid"), "rejected") && strstr(evaluate("settled.invalid"), "invalid JSON"), "resolved with invalid JSON: %s", evaluate("settled.invalid"));
    CHECK(!strcmp(evaluate("settled.reject"), "\"rejected rejected by the host\""), "rejected: %s", evaluate("settled.reject"));

    for (i = 0; i < sizeof results / sizeof *results; ++i) {
        zg_value *value = NULL;
        size_t length;
        const char *json;
        zg_view_evaluate(view, results[i].script, strlen(results[i].script), &value);
        json = zg_value_to_json(value, &length);
        CHECK(value && zg_value_type_of(value) == results[i].type && json && !strcmp(json, results[i].json),
            "evaluate %s: %s %s", results[i].script, value ? typeName(zg_value_type_of(value)) : "(none)", json ? json : "(null)");
        zg_value_release(value);
    }

    zg_runtime_destroy(runtime);
    printf(failures ? "FAILED\n" : "PASS\n");
    return failures ? 1 : 0;
}
