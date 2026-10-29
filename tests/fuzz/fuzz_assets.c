/* Asset archives (tools/assets.py, zg_context_mount_assets) from arbitrary bytes:
 * the parser accepts or refuses them as a whole, and an accepted archive serves
 * its files, compressed entries inflated on the first request, whole and by
 * range. The archive is used in place, as an application's static data is, and
 * the context is destroyed before the input goes away.
 *
 * The paths requested are the ones the seed archives (tests/fuzz/corpus/assets)
 * hold, so mutations of them keep reaching the files.
 */

#include "fuzz.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    zg_context_config config = { sizeof config };
    zg_context *context;
    char result[64];
    fuzzStart();
    config.ephemeral = 1;
    context = zg_context_create(fuzzRuntime, &config);
    if (zg_context_mount_assets(context, "app", data, size) == ZG_OK) {
        zg_view *view = fuzzView(context);
        fuzzLoad(view, "app://localhost/");
        /* Synchronous XHR: every load is done when evaluate returns. */
        fuzzEvaluate(view,
            "(() => { let n = 0;"
            "  for (const path of ['/', '/index.html', '/a.txt', '/z.bin', '/dir/', '/dir/b.js', '/missing'])"
            "    for (const range of [null, 'bytes=0-0', 'bytes=1-', 'bytes=-3', 'bytes=2-1']) {"
            "      try { const x = new XMLHttpRequest(); x.open(range ? 'GET' : 'HEAD', path, false);"
            "        if (range) x.setRequestHeader('Range', range);"
            "        x.send(); n += x.status + x.responseText.length; } catch (e) { }"
            "    }"
            "  return n; })()",
            result, sizeof result);
        zg_view_destroy(view);
        zg_runtime_update(fuzzRuntime);
    }
    zg_context_destroy(context);
    return 0;
}
