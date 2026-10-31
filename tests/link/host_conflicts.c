/* A host that brings its own copies of the engine's dependencies: the same ICU
   version (identical symbol names, linked from the stock ICU libraries) and its
   own mi_malloc. Linked together with the engine bundle, there must be no
   duplicate symbols, and each side must keep using its own copy
   (docs/design/host-coexistence.md, "Symbols in static linking"). */
#include <JavaScriptCore/JavaScript.h>
#include <unicode/uloc.h>
#include <unicode/ustring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int host_mi_malloc_calls;

/* Same name as mimalloc's entry point, which the engine uses on Windows. */
void *mi_malloc(size_t size)
{
    host_mi_malloc_calls++;
    return malloc(size);
}

static int check(const char *what, int ok)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

/* argv[1]: the engine's ICU data profile ("minimal", the default, or "full"). */
int main(int argc, char **argv)
{
    int ok = 1;
    int fullData = argc > 1 && !strcmp(argv[1], "full");

    /* Host ICU: stock data, every locale available. */
    UChar text[] = { 'z', 'e', 'r', 'o', 'g', 0 };
    ok &= check("host ICU u_strlen", u_strlen(text) == 5);
    ok &= check("host ICU has the stock data (sw locale available)",
                uloc_countAvailable() > 700);

    void *p = mi_malloc(16);
    ok &= check("host mi_malloc is the host's", p && host_mi_malloc_calls == 1);
    free(p);

    /* Engine: its own ICU with its own data (minimal: no Swahili), its own
       allocator. */
    JSGlobalContextRef ctx = JSGlobalContextCreate(NULL);
    JSStringRef source = JSStringCreateWithUTF8CString(
        "[new Intl.DateTimeFormat('it', {month: 'long', timeZone: 'UTC'}).format(0),"
        " Intl.NumberFormat.supportedLocalesOf(['sw', 'it']).join()].join('|')");
    JSValueRef value = JSEvaluateScript(ctx, source, NULL, NULL, 1, NULL);
    JSStringRelease(source);
    char buf[128] = "";
    if (value) {
        JSStringRef str = JSValueToStringCopy(ctx, value, NULL);
        JSStringGetUTF8CString(str, buf, sizeof buf);
        JSStringRelease(str);
    }
    printf("      engine: %s\n", buf);
    ok &= check(fullData ? "engine ICU uses its own full data" : "engine ICU uses its own minimal data",
                strcmp(buf, fullData ? "gennaio|sw,it" : "gennaio|it") == 0);
    ok &= check("engine did not call the host's mi_malloc", host_mi_malloc_calls == 1);
    JSGlobalContextRelease(ctx);

    return ok ? 0 : 1;
}
