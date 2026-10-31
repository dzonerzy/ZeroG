/* Binding latency (docs/design/measurable-goals.md): the cost of one call
 * from page JavaScript to native code, through the public API only.
 *
 *   js       an empty JavaScript function, for comparison
 *   sync     an empty synchronous binding (App.noop())
 *   async    await App.later(), its Promise resolved during the call
 *   thread   await App.thread(), its Promise resolved by a worker thread
 *   os       the same two thread switches without the engine, for comparison
 *
 * Usage: bindings [calls]   (default 200000; the async cases run a tenth)
 * Prints one line per case with the mean in microseconds, then PASS.
 */

#include "zerog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE semaphore_t;
static void semaphoreInit(semaphore_t *s) { *s = CreateSemaphoreW(NULL, 0, 1 << 30, NULL); }
static void semaphorePost(semaphore_t *s) { ReleaseSemaphore(*s, 1, NULL); }
static void semaphoreWait(semaphore_t *s) { WaitForSingleObject(*s, INFINITE); }
#else
#include <pthread.h>
#include <semaphore.h>
typedef sem_t semaphore_t;
static void semaphoreInit(semaphore_t *s) { sem_init(s, 0, 0); }
static void semaphorePost(semaphore_t *s) { sem_post(s); }
static void semaphoreWait(semaphore_t *s) { while (sem_wait(s)) { } }
#endif

static zg_runtime *runtime;
static char report[1024];

static zg_value *noop(zg_call *call, void *user)
{
    (void)call;
    (void)user;
    return NULL;
}

static zg_value *later(zg_call *call, void *user)
{
    zg_promise *promise;
    zg_value *value = zg_call_promise(call, &promise);
    (void)user;
    zg_promise_resolve(promise, "1", 1);
    return value;
}

/* One worker thread resolves each Promise it is handed. */
static zg_promise *handed;
static semaphore_t work;

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID argument)
#else
static void *worker(void *argument)
#endif
{
    (void)argument;
    for (;;) {
        semaphoreWait(&work);
        zg_promise_resolve(handed, "1", 1);
    }
}

/* The operating system's part of `thread`: a round trip between two threads
   (main -> worker -> main) through semaphores, without the engine. */
static semaphore_t ping, pong;

#ifdef _WIN32
static DWORD WINAPI echo(LPVOID argument)
#else
static void *echo(void *argument)
#endif
{
    (void)argument;
    for (;;) {
        semaphoreWait(&ping);
        semaphorePost(&pong);
    }
}

static double threadRoundTrip(long count)
{
    double start;
    long i;
    for (i = 0; i < 1000; ++i) {
        semaphorePost(&ping);
        semaphoreWait(&pong);
    }
    start = zg_runtime_now_ms();
    for (i = 0; i < count; ++i) {
        semaphorePost(&ping);
        semaphoreWait(&pong);
    }
    return (zg_runtime_now_ms() - start) * 1000 / count;
}

static zg_value *thread(zg_call *call, void *user)
{
    zg_value *value = zg_call_promise(call, &handed);
    (void)user;
    semaphorePost(&work);
    return value;
}

static zg_value *done(zg_call *call, void *user)
{
    size_t length;
    const char *text = zg_call_arg_string(call, 0, &length);
    (void)user;
    snprintf(report, sizeof report, "%.*s", (int)length, text ? text : "");
    zg_runtime_quit(runtime);
    return NULL;
}

int main(int argc, char **argv)
{
    long calls = argc > 1 ? atol(argv[1]) : 200000;
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context_config contextConfig = { sizeof contextConfig };
    zg_view_config viewConfig = { sizeof viewConfig };
    zg_context *context;
    zg_view *view;
    char page[4096];

    setvbuf(stdout, NULL, _IONBF, 0);
    semaphoreInit(&work);
    semaphoreInit(&ping);
    semaphoreInit(&pong);
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, echo, NULL, 0, NULL));
#else
    {
        pthread_t id;
        pthread_create(&id, NULL, worker, NULL);
        pthread_detach(id);
        pthread_create(&id, NULL, echo, NULL);
        pthread_detach(id);
    }
#endif

    runtime = zg_runtime_create(&runtimeConfig);
    context = zg_context_create(runtime, &contextConfig);
    viewConfig.width = 64;
    viewConfig.height = 64;
    view = zg_view_create(context, &viewConfig);
    zg_view_bind(view, "App.noop", noop, NULL);
    zg_view_bind(view, "App.later", later, NULL);
    zg_view_bind(view, "App.thread", thread, NULL);
    zg_view_bind(view, "App.done", done, NULL);

    /* Each case is warmed up first, so the JIT has compiled the loop. */
    snprintf(page, sizeof page,
        "<script>"
        "const N = %ld, M = Math.max(1, N / 10);"
        "function empty() { }"
        "function time(n, f) { f(Math.min(n, 1000)); const t = performance.now(); f(n); return (performance.now() - t) * 1000 / n; }"
        "async function timeAsync(n, f) { await f(Math.min(n, 1000)); const t = performance.now(); await f(n); return (performance.now() - t) * 1000 / n; }"
        "(async () => {"
        "  const js = time(N, n => { for (let i = 0; i < n; ++i) empty(); });"
        "  const sync = time(N, n => { for (let i = 0; i < n; ++i) App.noop(); });"
        "  const async_ = await timeAsync(M, async n => { for (let i = 0; i < n; ++i) await App.later(); });"
        "  const thread = await timeAsync(M, async n => { for (let i = 0; i < n; ++i) await App.thread(); });"
        "  App.done(JSON.stringify({ js, sync, async: async_, thread }));"
        "})();"
        "</script>", calls);
    zg_view_load_html(view, page, strlen(page), NULL);
    zg_runtime_run(runtime);

    printf("     %s\n", report);
    {
        double js, sync, async, threaded;
        if (sscanf(report, "{\"js\":%lf,\"sync\":%lf,\"async\":%lf,\"thread\":%lf}", &js, &sync, &async, &threaded) != 4) {
            printf("FAIL no measurement\n");
            return 1;
        }
        printf("js      %8.3f us per call (%ld calls)\n", js, calls);
        printf("sync    %8.3f us per call (%ld calls)\n", sync, calls);
        printf("async   %8.3f us per call (%ld calls)\n", async, calls / 10);
        printf("thread  %8.3f us per call (%ld calls)\n", threaded, calls / 10);
        printf("os      %8.3f us per round trip between two threads, without the engine\n", threadRoundTrip(calls / 10));
    }
    zg_runtime_destroy(runtime);
    printf("PASS\n");
    return 0;
}
