/* A host with a runtime of its own (docs/design/host-coexistence.md):
 * its own signal handlers and crash handler, installed before the engine starts,
 * as a game, a Go, .NET or Python runtime, or Crashpad/Sentry would have.
 *
 * - The host's handlers stay in place after the engine has started and run
 *   pages (POSIX: SIGUSR1, SIGUSR2 and the alternate signal stack; SIGSEGV
 *   may become JSC's fault handler, which calls the host's for faults that are
 *   not its own; Windows: the unhandled exception filter and a vectored
 *   handler).
 * - JSC's concurrent GC, under heavy allocation with the JIT on, works next to
 *   them, on a signal of its own (gc_signal), while the host keeps receiving
 *   its own signals or exceptions.
 * - A crash in host code reaches the host's crash handler, not the engine's
 *   (run in a child process: this program again, with "crash").
 * - Engine threads have names, for profilers and crash dumps.
 *
 * Linked against the engine bundle with include/zerog.h only (tools/build.py
 * linktest), as an application is.
 */

#include "zerog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CRASH_EXIT_CODE 42

static int check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

static zg_runtime *runtime;

/* ---- The host's handlers ---- */

#ifdef _WIN32
static volatile LONG hostExceptions;
#define HOST_EXCEPTION 0xE0005A47u

static LONG WINAPI hostVectoredHandler(EXCEPTION_POINTERS *info)
{
    if (info->ExceptionRecord->ExceptionCode == HOST_EXCEPTION) {
        InterlockedIncrement(&hostExceptions);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI hostCrashFilter(EXCEPTION_POINTERS *info)
{
    (void)info;
    fputs("host crash handler\n", stdout);
    fflush(stdout);
    ExitProcess(CRASH_EXIT_CODE);
}

static void installHostHandlers(void)
{
    SetUnhandledExceptionFilter(hostCrashFilter);
    AddVectoredExceptionHandler(1, hostVectoredHandler);
}

static int hostHandlersInPlace(void)
{
    /* Setting a filter returns the one in place: it must still be the host's. */
    LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(hostCrashFilter);
    return current == hostCrashFilter;
}

static void raiseHostSignal(void)
{
    RaiseException(HOST_EXCEPTION, 0, 0, NULL);
}

static long hostSignalCount(void)
{
    return hostExceptions;
}
#else
static volatile sig_atomic_t hostUsr1, hostUsr2;
static char alternateStack[256 * 1024];

static void hostUsr1Handler(int signal) { (void)signal; ++hostUsr1; }
static void hostUsr2Handler(int signal) { (void)signal; ++hostUsr2; }

static void hostCrashHandler(int signal, siginfo_t *info, void *context)
{
    static const char message[] = "host crash handler\n";
    (void)signal;
    (void)info;
    (void)context;
    if (write(1, message, sizeof message - 1) < 0) { }
    _exit(CRASH_EXIT_CODE);
}

static void installHostHandlers(void)
{
    struct sigaction action;
    stack_t stack;
    stack.ss_sp = alternateStack;
    stack.ss_size = sizeof alternateStack;
    stack.ss_flags = 0;
    sigaltstack(&stack, NULL);

    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    action.sa_handler = hostUsr1Handler;
    sigaction(SIGUSR1, &action, NULL);
    action.sa_handler = hostUsr2Handler;
    sigaction(SIGUSR2, &action, NULL);
    action.sa_handler = NULL;
    action.sa_sigaction = hostCrashHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
}

static int hostHandlersInPlace(void)
{
    struct sigaction usr1, usr2, segv;
    stack_t stack;
    sigaction(SIGUSR1, NULL, &usr1);
    sigaction(SIGUSR2, NULL, &usr2);
    sigaction(SIGSEGV, NULL, &segv);
    sigaltstack(NULL, &stack);
    /* SIGSEGV may be JSC's: WTF's fault handler (threads/Signals.cpp) keeps the
       handler it replaced and calls it for every fault that is not JSC's own,
       which the crash child checks. The others must stay the host's. */
    printf("      SIGUSR1 %s, SIGUSR2 %s, SIGSEGV %s, alternate stack %s\n",
        usr1.sa_handler == hostUsr1Handler ? "host's" : "replaced", usr2.sa_handler == hostUsr2Handler ? "host's" : "replaced",
        segv.sa_sigaction == hostCrashHandler ? "host's" : "JSC's, chaining to the host's",
        stack.ss_sp == alternateStack ? "host's" : "replaced");
    return usr1.sa_handler == hostUsr1Handler && usr2.sa_handler == hostUsr2Handler && stack.ss_sp == alternateStack;
}

static void raiseHostSignal(void)
{
    kill(getpid(), SIGUSR1);
    kill(getpid(), SIGUSR2);
}

static long hostSignalCount(void)
{
    return hostUsr1 < hostUsr2 ? hostUsr1 : hostUsr2;
}
#endif

/* ---- The engine at work ---- */

static void sleepMilliseconds(int milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    usleep(milliseconds * 1000);
#endif
}

/* A page allocating a lot (objects, arrays, strings, kept and dropped) so the
   GC runs concurrently, with the JIT compiling the loop. */
static const char allocatingPage[] =
    "<!DOCTYPE html><script>"
    "let kept = []; window.rounds = 0;"
    "function churn() {"
    "  for (let i = 0; i < 20000; ++i) {"
    "    const o = { i, s: 'item' + i, a: [i, i * 2, i * 3] };"
    "    if (i % 50 === 0) kept.push(o);"
    "  }"
    "  if (kept.length > 20000) kept = kept.slice(10000);"
    "  ++window.rounds; setTimeout(churn, 0);"
    "}"
    "churn();"
    "</script>";

static zg_view *startPage(zg_context *context)
{
    zg_view_config viewConfig = { sizeof viewConfig };
    zg_view *view;
    viewConfig.width = 200;
    viewConfig.height = 100;
    view = zg_view_create(context, &viewConfig);
    if (view)
        zg_view_load_html(view, allocatingPage, sizeof allocatingPage - 1, NULL);
    return view;
}

static void pump(zg_view *view, int milliseconds, int signalHost)
{
    double end = zg_runtime_now_ms() + milliseconds;
    while (zg_runtime_now_ms() < end) {
        zg_frame frame;
        zg_runtime_update(runtime);
        zg_view_render(view, zg_runtime_now_ms());
        if (zg_surface_acquire(zg_view_surface(view), &frame) == ZG_OK)
            zg_surface_release(zg_view_surface(view), &frame);
        if (signalHost)
            raiseHostSignal();
        sleepMilliseconds(5);
    }
}

/* How many allocation rounds the page has run. */
static long rounds(zg_view *view)
{
    static const char script[] = "window.rounds";
    zg_value *value = NULL;
    long count = -1;
    if (zg_view_evaluate(view, script, sizeof script - 1, &value) == ZG_OK && value) {
        const char *json = zg_value_to_json(value, NULL);
        count = json ? strtol(json, NULL, 10) : -1;
        zg_value_release(value);
    }
    return count;
}

/* ---- Thread names ---- */

static int namedEngineThreads(void)
{
    int named = 0;
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry;
    entry.dwSize = sizeof entry;
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        HANDLE thread;
        PWSTR description = NULL;
        if (entry.th32OwnerProcessID != GetCurrentProcessId())
            continue;
        thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread && SUCCEEDED(GetThreadDescription(thread, &description)) && description && *description) {
            if (named < 6)
                printf("      thread: %ls\n", description);
            ++named;
        }
        if (description)
            LocalFree(description);
        if (thread)
            CloseHandle(thread);
    }
    CloseHandle(snapshot);
#else
    char self[64] = "";
    FILE *file = fopen("/proc/self/comm", "r");
    DIR *tasks;
    struct dirent *task;
    if (file) {
        if (!fgets(self, sizeof self, file))
            self[0] = 0;
        fclose(file);
    }
    tasks = opendir("/proc/self/task");
    while (tasks && (task = readdir(tasks))) {
        char path[128], name[64] = "";
        if (task->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", task->d_name);
        file = fopen(path, "r");
        if (file) {
            if (!fgets(name, sizeof name, file))
                name[0] = 0;
            fclose(file);
        }
        /* An unnamed thread keeps the program's name. */
        if (name[0] && strcmp(name, self)) {
            if (named < 6)
                printf("      thread: %s", name);
            ++named;
        }
    }
    if (tasks)
        closedir(tasks);
#endif
    return named;
}

/* ---- The crash child ---- */

static int runCrashChild(void)
{
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context_config contextConfig = { sizeof contextConfig };
    zg_context *context;
    zg_view *view;
    volatile int *nothing = NULL;
    installHostHandlers();
    runtime = zg_runtime_create(&runtimeConfig);
    contextConfig.ephemeral = 1;
    context = zg_context_create(runtime, &contextConfig);
    view = startPage(context);
    pump(view, 300, 0);
    /* The host's bug, with the engine running. */
    *nothing = 1;
    return 0;
}

static int crashChildReachesHostHandler(const char *self)
{
#ifdef _WIN32
    wchar_t path[MAX_PATH], command[MAX_PATH + 16];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD code = 0;
    (void)self;
    GetModuleFileNameW(NULL, path, MAX_PATH);
    swprintf(command, MAX_PATH + 16, L"\"%ls\" crash", path);
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    /* No Windows Error Reporting dialog if the host's handler were bypassed. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (!CreateProcessW(path, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process))
        return 0;
    WaitForSingleObject(process.hProcess, 60000);
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    printf("      child exit code %lu\n", (unsigned long)code);
    return code == CRASH_EXIT_CODE;
#else
    int status = 0;
    pid_t child = fork();
    if (!child) {
        execl("/proc/self/exe", self, "crash", (char *)NULL);
        _exit(1);
    }
    if (child < 0 || waitpid(child, &status, 0) != child)
        return 0;
    if (WIFSIGNALED(status))
        printf("      child killed by signal %d\n", WTERMSIG(status));
    else
        printf("      child exit code %d\n", WEXITSTATUS(status));
    return WIFEXITED(status) && WEXITSTATUS(status) == CRASH_EXIT_CODE;
#endif
}

int main(int argc, char **argv)
{
    zg_runtime_config runtimeConfig = { sizeof runtimeConfig };
    zg_context_config contextConfig = { sizeof contextConfig };
    zg_context *context;
    zg_view *view;
    long before, after, signalsBefore;
    int ok = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "crash"))
        return runCrashChild();

    installHostHandlers();
    runtime = zg_runtime_create(&runtimeConfig);
    contextConfig.ephemeral = 1;
    context = zg_context_create(runtime, &contextConfig);
    view = startPage(context);
    if (!check(runtime && context && view, "the engine starts in a host with its own handlers"))
        return 1;

    /* POSIX: had JSC's GC taken SIGUSR1 or SIGUSR2, WTF would have replaced the
       host's handler for it, which hostHandlersInPlace() below would see. */
    pump(view, 500, 0);
    before = rounds(view);
    signalsBefore = hostSignalCount();
    /* Allocation and concurrent GC while the host raises its own signals. */
    pump(view, 2000, 1);
    zg_runtime_collect_garbage(runtime);
    after = rounds(view);
    printf("      %ld allocation rounds, %ld host signals\n", after - before, hostSignalCount() - signalsBefore);
    ok &= check(after > before + 5, "pages allocate and collect with the host's handlers installed");
    ok &= check(hostSignalCount() > signalsBefore + 50, "the host keeps receiving its own signals");
    ok &= check(hostHandlersInPlace(), "the host's handlers are still the ones installed (the GC's signal is its own)");

    {
        int named = namedEngineThreads();
        printf("      %d named threads\n", named);
        ok &= check(named >= 3, "engine threads have names");
    }

    zg_view_destroy(view);
    zg_context_destroy(context);
    zg_runtime_destroy(runtime);

    ok &= check(crashChildReachesHostHandler(argv[0]), "a crash in host code reaches the host's crash handler");
    printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
