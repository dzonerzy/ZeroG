/* Values crossing the binding boundary, from arbitrary bytes:
 *   - zg_value_json returned to JavaScript: accepted only if it is JSON, and then
 *     zg_call_arg_json gives back what JSON.stringify gives;
 *   - zg_value_string_len: valid UTF-8 comes back from zg_call_arg_string byte
 *     for byte (embedded NULs included); anything else must not crash;
 *   - a Promise resolved with the bytes as JSON;
 *   - every zg_call_arg_* reader on what JavaScript makes of them.
 */

#include "fuzz.h"

static zg_view *view;
static const uint8_t *input;
static size_t inputSize;

/* Strict UTF-8: no overlong forms, surrogates or code points past U+10FFFF. */
static int isUTF8(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t length, k;
        uint32_t code;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            length = 2, code = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF)
            length = 3, code = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4)
            length = 4, code = c & 0x07;
        else
            return 0;
        if (i + length > n)
            return 0;
        for (k = 1; k < length; ++k) {
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
            code = (code << 6) | (s[i + k] & 0x3F);
        }
        if ((length == 3 && code < 0x800) || (length == 4 && (code < 0x10000 || code > 0x10FFFF)) || (code >= 0xD800 && code <= 0xDFFF))
            return 0;
        i += length;
    }
    return 1;
}

static zg_value *asJSON(zg_call *call, void *user)
{
    (void)user;
    return zg_value_json(call, (const char *)input, inputSize);
}

static zg_value *asString(zg_call *call, void *user)
{
    (void)user;
    return zg_value_string_len(call, (const char *)input, inputSize);
}

static zg_value *asPromise(zg_call *call, void *user)
{
    zg_promise *promise = NULL;
    zg_value *value = zg_call_promise(call, &promise);
    (void)user;
    zg_promise_resolve(promise, (const char *)input, inputSize);
    return value;
}

/* JSON of argument 0, as the host reads it. */
static zg_value *readJSON(zg_call *call, void *user)
{
    size_t length = 0;
    const char *json = zg_call_arg_json(call, 0, &length);
    (void)user;
    return json ? zg_value_string_len(call, json, length) : zg_value_null(call);
}

/* Whether argument 0, read as a string, is the input itself. */
static zg_value *isInput(zg_call *call, void *user)
{
    size_t length = 0;
    const char *string = zg_call_arg_string(call, 0, &length);
    (void)user;
    return zg_value_bool(call, string && length == inputSize && !memcmp(string, input, inputSize));
}

/* Every reader on every argument; the results are not checked, only survived. */
static zg_value *readAll(zg_call *call, void *user)
{
    size_t i, length, total = 0;
    (void)user;
    for (i = 0; i <= zg_call_argc(call); ++i) {
        total += (size_t)zg_call_arg_type(call, i);
        total += (size_t)zg_call_arg_bool(call, i);
        total += (size_t)(zg_call_arg_number(call, i) == 0);
        total += (size_t)zg_call_arg_int64(call, i);
        if (zg_call_arg_string(call, i, &length))
            total += length;
        if (zg_call_arg_json(call, i, &length))
            total += length;
    }
    return zg_value_number(call, (double)total);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char result[64];
    if (!view) {
        zg_context_config config = { sizeof config };
        zg_context *context;
        fuzzStart();
        config.ephemeral = 1;
        context = zg_context_create(fuzzRuntime, &config);
        /* app:// with files is a trusted origin: its pages get the bindings. */
        zg_context_add_file(context, "app", "/index.html", "", 0, "text/html", NULL, NULL);
        view = fuzzView(context);
        zg_view_bind(view, "Fuzz.json", asJSON, NULL);
        zg_view_bind(view, "Fuzz.string", asString, NULL);
        zg_view_bind(view, "Fuzz.promise", asPromise, NULL);
        zg_view_bind(view, "Fuzz.readJSON", readJSON, NULL);
        zg_view_bind(view, "Fuzz.isInput", isInput, NULL);
        zg_view_bind(view, "Fuzz.readAll", readAll, NULL);
        fuzzLoad(view, "app://localhost/");
    }
    input = data;
    inputSize = size;

    /* "true": JSON the engine accepted and reads back as JSON.stringify writes it;
       "false": it did not round-trip; "null": not accepted (a TypeError). */
    fuzzEvaluate(view,
        "(() => { let v; try { v = Fuzz.json(); } catch (e) { return null; }"
        "  const s = JSON.stringify(v); return s === undefined || Fuzz.readJSON(v) === s; })()",
        result, sizeof result);
    FUZZ_ASSERT(strcmp(result, "false"), "zg_call_arg_json gives what JSON.stringify gives");

    fuzzEvaluate(view, "Fuzz.isInput(Fuzz.string())", result, sizeof result);
    if (isUTF8(data, size))
        FUZZ_ASSERT(!strcmp(result, "true"), "valid UTF-8 comes back byte for byte");

    fuzzEvaluate(view,
        "(() => { const s = Fuzz.string(); let j; try { j = Fuzz.json(); } catch (e) { j = s; }"
        "  Fuzz.promise().then(v => Fuzz.readAll(v), e => Fuzz.readAll(e.message));"
        "  return Fuzz.readAll(s, j, [s, j], { s, j }, s.length, BigInt(s.length) ** 40n); })()",
        result, sizeof result);
    zg_runtime_update(fuzzRuntime);
    return 0;
}
