/* What the fuzz targets share: one runtime for the whole process, helpers to run
 * script and wait, and, without libFuzzer, a main that replays inputs from files
 * (the corpus as a regression test, on every OS).
 *
 * Each target defines LLVMFuzzerTestOneInput. With libFuzzer (the ASan build on
 * Windows, ZEROG_LIBFUZZER) libFuzzer drives it, with its usual arguments:
 *   ZeroGFuzzValues build/fuzz/values tests/fuzz/corpus/values -max_total_time=600
 * Without it, each file given is one input:
 *   ZeroGFuzzValues tests/fuzz/corpus/values/<files>
 */

#ifndef ZEROG_FUZZ_H
#define ZEROG_FUZZ_H

#include "zerog.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static zg_runtime *fuzzRuntime;

/* An invariant broken: a crash, so that libFuzzer keeps the input. */
#define FUZZ_ASSERT(condition, message) do {                        \
        if (!(condition)) {                                         \
            fprintf(stderr, "fuzz invariant broken: %s\n", message); \
            abort();                                                \
        }                                                           \
    } while (0)

static void fuzzStart(void)
{
    zg_runtime_config config = { sizeof config };
    if (fuzzRuntime)
        return;
    config.log_level = ZG_LOG_ERROR;
    fuzzRuntime = zg_runtime_create(&config);
    FUZZ_ASSERT(fuzzRuntime, "runtime created");
}

static zg_view *fuzzView(zg_context *context)
{
    zg_view_config config = { sizeof config };
    zg_view *view;
    config.width = 64;
    config.height = 64;
    view = zg_view_create(context, &config);
    FUZZ_ASSERT(view, "view created");
    return view;
}

/* An empty document at `base_url`, there when this returns; bindings go before
   it (zg_view_bind). */
static void fuzzLoad(zg_view *view, const char *baseURL)
{
    static const char page[] = "<!DOCTYPE html><title>fuzz</title>";
    FUZZ_ASSERT(zg_view_load_html(view, page, sizeof page - 1, baseURL) == ZG_OK, "page loaded");
}

/* Runs `script`; returns its JSON in `out` (truncated), or "" if it threw. */
static void fuzzEvaluate(zg_view *view, const char *script, char *out, size_t outSize)
{
    zg_value *value = NULL;
    size_t length = 0;
    const char *json;
    out[0] = 0;
    if (zg_view_evaluate(view, script, strlen(script), &value) != ZG_OK)
        return;
    json = zg_value_to_json(value, &length);
    if (json)
        snprintf(out, outSize, "%.*s", (int)length, json);
    zg_value_release(value);
}

#ifdef ZEROG_LIBFUZZER
/* libFuzzer is linked without its main (port/api/CMakeLists.txt). */
int LLVMFuzzerRunDriver(int *argc, char ***argv, int (*callback)(const uint8_t *data, size_t size));

int main(int argc, char **argv)
{
    return LLVMFuzzerRunDriver(&argc, &argv, LLVMFuzzerTestOneInput);
}
#else
int main(int argc, char **argv)
{
    int i, ran = 0;
    for (i = 1; i < argc; ++i) {
        FILE *file = fopen(argv[i], "rb");
        unsigned char *data;
        long size;
        if (!file) {
            fprintf(stderr, "cannot open %s\n", argv[i]);
            return 1;
        }
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fseek(file, 0, SEEK_SET);
        /* Its own allocation, exactly the input's size, as libFuzzer does. */
        data = (unsigned char *)malloc(size ? (size_t)size : 1);
        if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 1;
        }
        fclose(file);
        LLVMFuzzerTestOneInput(data, (size_t)size);
        free(data);
        ++ran;
    }
    printf("PASS %d inputs\n", ran);
    return 0;
}
#endif

#endif
