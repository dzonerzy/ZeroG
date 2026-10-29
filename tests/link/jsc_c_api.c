/* Link test: a plain C program linked against the single engine archive
   (plus ICU and system libraries) evaluates JavaScript through the JSC C API
   and calls back into a native function, the basis of the zg_* bindings. */
#include <JavaScriptCore/JavaScript.h>
#include <stdio.h>
#include <string.h>

static JSValueRef native_add(JSContextRef ctx, JSObjectRef fn, JSObjectRef self,
                             size_t argc, const JSValueRef argv[], JSValueRef *exception)
{
    (void)fn; (void)self;
    double a = argc > 0 ? JSValueToNumber(ctx, argv[0], exception) : 0;
    double b = argc > 1 ? JSValueToNumber(ctx, argv[1], exception) : 0;
    return JSValueMakeNumber(ctx, a + b);
}

static int check(JSGlobalContextRef ctx, const char *script, const char *expected)
{
    JSStringRef source = JSStringCreateWithUTF8CString(script);
    JSValueRef exception = NULL;
    JSValueRef value = JSEvaluateScript(ctx, source, NULL, NULL, 1, &exception);
    JSStringRelease(source);
    JSStringRef str = JSValueToStringCopy(ctx, exception ? exception : value, NULL);
    char buf[256];
    JSStringGetUTF8CString(str, buf, sizeof buf);
    JSStringRelease(str);
    int ok = !exception && strcmp(buf, expected) == 0;
    printf("%s  %s => %s\n", ok ? "ok  " : "FAIL", script, buf);
    return ok;
}

/* argv[1]: ICU data profile linked in ("minimal" or "full"), selects the
   expected locale availability. */
int main(int argc, char **argv)
{
    int full_icu = argc > 1 && strcmp(argv[1], "full") == 0;
    JSGlobalContextRef ctx = JSGlobalContextCreate(NULL);
    JSStringRef name = JSStringCreateWithUTF8CString("nativeAdd");
    JSObjectRef fn = JSObjectMakeFunctionWithCallback(ctx, name, native_add);
    JSObjectSetProperty(ctx, JSContextGetGlobalObject(ctx), name, fn, kJSPropertyAttributeNone, NULL);
    JSStringRelease(name);

    int ok = 1;
    ok &= check(ctx, "nativeAdd(40, 2)", "42");
    ok &= check(ctx, "let s = 0; for (let i = 0; i < 1e7; i++) s += i; s", "49999995000000");
    ok &= check(ctx, "new Intl.NumberFormat('de-DE').format(1234567.891)", "1.234.567,891");
    ok &= check(ctx, "JSON.stringify({a: [1, 2, {b: null}]})", "{\"a\":[1,2,{\"b\":null}]}");

    /* ICU data (deps/icu-data.json): kept locales work, dropped ones are not advertised. */
    ok &= check(ctx, "new Intl.DateTimeFormat('it-IT', {dateStyle: 'full', timeZone: 'Europe/Rome'}).format(0)",
                "gioved\xc3\xac 1 gennaio 1970");
    ok &= check(ctx, "new Intl.DateTimeFormat('ja-JP', {timeZone: 'Asia/Tokyo', hour: 'numeric'}).format(0)",
                "9\xe6\x99\x82");
    ok &= check(ctx, "Intl.NumberFormat.supportedLocalesOf(['it', 'sw', 'ja']).join()",
                full_icu ? "it,sw,ja" : "it,ja");
    /* Segmentation dictionaries are kept in every profile: Thai has no spaces. */
    ok &= check(ctx, "[...new Intl.Segmenter('th', {granularity: 'word'}).segment('\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5\xe0\xb8\x84\xe0\xb8\xa3\xe0\xb8\xb1\xe0\xb8\x9a')].length",
                "2");

    JSGlobalContextRelease(ctx);
    return ok ? 0 : 1;
}
