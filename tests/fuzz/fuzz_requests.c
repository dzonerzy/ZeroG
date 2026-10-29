/* Requests to app:// from arbitrary bytes. The input is three parts separated by
 * newlines: a path, a Range header, and the rest a file's contents. The path is
 * added as a file (zg_context_add_file normalizes or refuses it) and set as the
 * fallback, then requested from the page, as given and resolved as a URL, with
 * the Range header, and the answer checked against the file when it is whole.
 */

#include "fuzz.h"

static const uint8_t *parts[3];
static size_t partSizes[3];

static zg_value *part(zg_call *call, void *user)
{
    int64_t index = zg_call_arg_int64(call, 0);
    (void)user;
    if (index < 0 || index > 2)
        return zg_value_null(call);
    return zg_value_string_len(call, (const char *)parts[index], partSizes[index]);
}

/* Whether argument 0, read as a string, is the file's contents. */
static zg_value *isContents(zg_call *call, void *user)
{
    size_t length = 0;
    const char *string = zg_call_arg_string(call, 0, &length);
    (void)user;
    return zg_value_bool(call, string && length == partSizes[2] && !memcmp(string, parts[2], length));
}

static void split(const uint8_t *data, size_t size)
{
    size_t i, start = 0;
    int index = 0;
    for (i = 0; i < size && index < 2; ++i) {
        if (data[i] == '\n') {
            parts[index] = data + start;
            partSizes[index] = i - start;
            ++index;
            start = i + 1;
        }
    }
    for (; index < 3; ++index) {
        parts[index] = data + start;
        partSizes[index] = size - start;
        start = size;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    zg_context_config config = { sizeof config };
    zg_context *context;
    zg_view *view;
    char path[512], result[64];
    zg_result added;
    fuzzStart();
    split(data, size);
    snprintf(path, sizeof path, "%.*s", (int)(partSizes[0] < 500 ? partSizes[0] : 500), (const char *)parts[0]);

    config.ephemeral = 1;
    context = zg_context_create(fuzzRuntime, &config);
    zg_context_add_file(context, "app", "/index.html", "", 0, "text/html", NULL, NULL);
    added = zg_context_add_file(context, "app", path, parts[2], partSizes[2], NULL, NULL, NULL);
    zg_context_set_fallback(context, "app", path);

    view = fuzzView(context);
    zg_view_bind(view, "Fuzz.part", part, NULL);
    zg_view_bind(view, "Fuzz.isContents", isContents, NULL);
    fuzzLoad(view, "app://localhost/");
    fuzzEvaluate(view,
        "(() => { const path = Fuzz.part(0), range = Fuzz.part(1); let whole = null;"
        "  for (const url of [path, encodeURI(path), 'app://localhost' + path, '/settings/' + path]) {"
        "    for (const r of [null, range]) {"
        "      try { const x = new XMLHttpRequest(); x.open('GET', url, false);"
        "        if (r !== null) x.setRequestHeader('Range', r);"
        "        x.send();"
        "        if (r === null && url === 'app://localhost' + path && x.status === 200 && whole === null)"
        "          whole = Fuzz.isContents(x.responseText);"
        "      } catch (e) { }"
        "    }"
        "  }"
        "  return whole; })()",
        result, sizeof result);
    /* A file that was added, asked for by its own path, is served as it is. Text is
       decoded as UTF-8 by XHR, so only plain ASCII contents compare byte for byte. */
    /* The page asks for the whole part; the file was added at it up to a NUL. */
    if (added == ZG_OK && strlen(path) == partSizes[0] && path[0] == '/' && !strstr(path, "%") && !strstr(path, "?") && !strstr(path, "#")) {
        size_t i;
        int ascii = 1;
        for (i = 0; i < partSizes[2]; ++i)
            ascii &= parts[2][i] >= 0x20 && parts[2][i] < 0x7f;
        for (i = 0; path[i]; ++i)
            ascii &= (unsigned char)path[i] > 0x20 && (unsigned char)path[i] < 0x7f;
        if (ascii)
            FUZZ_ASSERT(!strcmp(result, "true"), "an added file is served whole");
    }
    zg_view_destroy(view);
    zg_runtime_update(fuzzRuntime);
    zg_context_destroy(context);
    return 0;
}
