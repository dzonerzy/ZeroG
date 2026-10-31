/* Memory at rest (docs/design/measurable-goals.md), through the public API only:
 * one scenario per process, measured after its page has loaded, rendered and
 * settled, and the JavaScript heap has been collected.
 *
 *   baseline  a runtime and a context, no view
 *   cpu       one empty view on a CPU surface
 *   texture   one empty view on a GPU surface (D3D11 on Windows, dma-bufs on Linux)
 *   window    one empty window
 *   page      one CPU view with a heavier page (2000 elements, styles, script state)
 *
 * Usage: memory <scenario>
 * Prints the process's memory as the OS counts it (Windows: private bytes and
 * working set; Linux: RSS, PSS and private memory from /proc/self/smaps_rollup)
 * and the engine's own counters (zg_memory_stats), then one JSON line for
 * tools/memory.py, then PASS.
 */

#include "zerog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define PSAPI_VERSION 2 /* GetProcessMemoryInfo from kernel32 */
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

static zg_runtime *runtime;

static void sleepMilliseconds(int milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    usleep(milliseconds * 1000);
#endif
}

typedef struct {
    double resident;  /* working set / RSS */
    double private_;  /* private bytes / private (clean + dirty) */
    double shared;    /* Linux: PSS; Windows: 0 */
} process_memory;

static process_memory processMemory(void)
{
    process_memory memory = { 0, 0, 0 };
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof counters)) {
        memory.resident = (double)counters.WorkingSetSize;
        memory.private_ = (double)counters.PrivateUsage;
    }
#else
    char line[256];
    FILE *file = fopen("/proc/self/smaps_rollup", "r");
    while (file && fgets(line, sizeof line, file)) {
        double kb;
        if (sscanf(line, "Rss: %lf kB", &kb) == 1)
            memory.resident = kb * 1024;
        else if (sscanf(line, "Pss: %lf kB", &kb) == 1)
            memory.shared = kb * 1024;
        else if (sscanf(line, "Private_Clean: %lf kB", &kb) == 1 || sscanf(line, "Private_Dirty: %lf kB", &kb) == 1)
            memory.private_ += kb * 1024;
    }
    if (file)
        fclose(file);
#endif
    return memory;
}

static const char emptyPage[] = "<!DOCTYPE html><html><body></body></html>";

/* 2000 elements in a flex layout with text, borders and rounded corners, and
   some script state, as an application's frontend might have. */
static const char heavyPage[] =
    "<!DOCTYPE html><html><head><style>"
    "body { font: 14px sans-serif; display: flex; flex-wrap: wrap; gap: 4px; margin: 8px; }"
    ".card { width: 120px; padding: 6px; border: 1px solid #ccd; border-radius: 6px; background: linear-gradient(#fff, #eef); }"
    ".card b { display: block; color: #335; }"
    "</style></head><body><script>"
    "const items = [];"
    "for (let i = 0; i < 2000; ++i) {"
    "  const card = document.createElement('div'); card.className = 'card';"
    "  card.innerHTML = '<b>Item ' + i + '</b><span>' + 'lorem ipsum '.repeat(3) + '</span>';"
    "  document.body.appendChild(card);"
    "  items.push({ id: i, name: 'item' + i, tags: ['a', 'b', 'c'], value: Math.random() });"
    "}"
    "</script></body></html>";

/* Loads `page` into `view` and renders it for `milliseconds` on the
   application's clock, taking and giving back each frame. */
static void settle(zg_view *view, int milliseconds)
{
    double end = zg_runtime_now_ms() + milliseconds;
    while (zg_runtime_now_ms() < end) {
        zg_frame frame;
        zg_runtime_update(runtime);
        zg_view_render(view, zg_runtime_now_ms());
        if (zg_surface_acquire(zg_view_surface(view), &frame) == ZG_OK)
            zg_surface_release(zg_view_surface(view), &frame);
        sleepMilliseconds(16);
    }
}

static zg_value *done(zg_call *call, void *user)
{
    (void)call;
    (void)user;
    zg_runtime_quit(runtime);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *scenario = argc > 1 ? argv[1] : "cpu";
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context_config contextConfig = { sizeof contextConfig };
    zg_view_config viewConfig = { sizeof viewConfig };
    zg_surface_config surfaceConfig = { sizeof surfaceConfig };
    zg_context *context;
    zg_view *view = NULL;
    zg_window *window = NULL;
    zg_memory_stats stats;
    process_memory memory;

    setvbuf(stdout, NULL, _IONBF, 0);
    runtime = zg_runtime_create(&runtimeConfig);
    contextConfig.ephemeral = 1;
    context = zg_context_create(runtime, &contextConfig);
    if (!runtime || !context) {
        printf("FAIL no runtime: %s\n", zg_last_error());
        return 1;
    }
    viewConfig.width = 800;
    viewConfig.height = 600;

    if (!strcmp(scenario, "baseline")) {
        zg_runtime_update(runtime);
    } else if (!strcmp(scenario, "cpu") || !strcmp(scenario, "page") || !strcmp(scenario, "texture")) {
        int heavy = !strcmp(scenario, "page");
        if (!strcmp(scenario, "texture")) {
#ifdef _WIN32
            surfaceConfig.kind = ZG_SURFACE_D3D11; /* on a device the engine creates */
#else
            surfaceConfig.kind = ZG_SURFACE_VULKAN; /* dma-bufs on the engine's own display */
#endif
            viewConfig.surface = &surfaceConfig;
        }
        view = zg_view_create(context, &viewConfig);
        if (!view && viewConfig.surface) {
            /* No GPU surface here (Linux without a device that makes dma-bufs). */
            printf("skipped %s: %s\nPASS\n", scenario, zg_last_error());
            return 0;
        }
        if (!view) {
            printf("FAIL no view: %s\n", zg_last_error());
            return 1;
        }
        zg_view_load_html(view, heavy ? heavyPage : emptyPage, heavy ? sizeof heavyPage - 1 : sizeof emptyPage - 1, NULL);
        settle(view, 1500);
    } else if (!strcmp(scenario, "window")) {
        static const char windowPage[] = "<!DOCTYPE html><html><body><script>setTimeout(() => App.done(), 1500)</script></body></html>";
        zg_window_config windowConfig = { sizeof windowConfig };
        windowConfig.title = "ZeroG memory";
        windowConfig.width = 800;
        windowConfig.height = 600;
        window = zg_window_create(context, &windowConfig);
        if (!window) {
            printf("FAIL no window: %s\n", zg_last_error());
            return 1;
        }
        view = zg_window_view(window);
        zg_view_bind(view, "App.done", done, NULL);
        zg_view_load_html(view, windowPage, sizeof windowPage - 1, NULL);
        zg_runtime_run(runtime);
    } else {
        printf("FAIL unknown scenario %s (baseline, cpu, texture, window, page)\n", scenario);
        return 1;
    }

    /* At rest: garbage collected, the page idle for a moment. */
    zg_runtime_collect_garbage(runtime);
    if (view && !window)
        settle(view, 300);
    else
        zg_runtime_update(runtime);
    memset(&stats, 0, sizeof stats);
    stats.size = sizeof stats;
    zg_runtime_memory_stats(runtime, &stats);
    memory = processMemory();

#define MB(x) ((double)(x) / (1024.0 * 1024.0))
#ifdef _WIN32
    printf("%-9s private %7.1f MB  working set %7.1f MB\n", scenario, MB(memory.private_), MB(memory.resident));
#else
    printf("%-9s private %7.1f MB  RSS %7.1f MB  PSS %7.1f MB\n", scenario, MB(memory.private_), MB(memory.resident), MB(memory.shared));
#endif
    printf("          JS heap %.1f MB live of %.1f MB, resource cache %.1f MB, surfaces %.1f MB, assets %.1f MB\n",
        MB(stats.js_heap_bytes), MB(stats.js_heap_capacity), MB(stats.resource_cache_bytes), MB(stats.surface_bytes), MB(stats.asset_bytes));
    printf("{\"scenario\":\"%s\",\"private\":%.0f,\"resident\":%.0f,\"pss\":%.0f,\"js_heap\":%lu,\"js_heap_capacity\":%lu,\"resource_cache\":%lu,\"surfaces\":%lu,\"assets\":%lu}\n",
        scenario, memory.private_, memory.resident, memory.shared, (unsigned long)stats.js_heap_bytes, (unsigned long)stats.js_heap_capacity,
        (unsigned long)stats.resource_cache_bytes, (unsigned long)stats.surface_bytes, (unsigned long)stats.asset_bytes);

    if (window)
        zg_window_destroy(window);
    else if (view)
        zg_view_destroy(view);
    zg_context_destroy(context);
    zg_runtime_destroy(runtime);
    printf("PASS\n");
    return 0;
}
