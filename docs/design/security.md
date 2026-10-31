# Security

With a single process **there is no sandbox for web content**: a memory bug in WebCore or JSC exploited by a page becomes code execution with the application's privileges. The trust model is therefore clear-cut: the engine is designed for application content, and external content is an explicit exception, documented as high risk.

## Attack surface

| Surface | Risk | Mitigation |
| --- | --- | --- |
| WebCore and JSC | Memory bugs exploitable by malicious content, without sandbox | External content disabled by default, JIT can be disabled, unnecessary web features excluded from the build |
| Host bindings | Native calls from unexpected origins | Bindings only for trusted origins (app assets; dev server only in the dev profile; optional host policy), only in the main frame by default, origin check for every call |
| `app://` scheme | Path traversal, MIME sniffing | Read-only archive, no filesystem access, explicit MIME types |
| Navigation | App page taken to an external site | Navigation outside `app://` blocked by default; external links open in the system browser |
| External content (navigation and iframes) | Untrusted code in the same process as the app | Disabled by default. It is enabled only with an explicit developer flag (`allow_external_content` in `zg_context_config`), who takes responsibility for it: the engine does not add a sandbox and security depends on that of upstream WebKit and on the speed of updates. Bindings are still not installed for untrusted origins; the JIT follows its own option and is not turned off automatically |

## Surface reduction

The build profile is also a security tool: every excluded feature (WebAssembly, WebGL, XSLT, media) is code that cannot be attacked. The minimal profile is the one recommended for applications that do not load external content.

## JIT

The JIT is a per-process choice, not per-view: JSC's options are global and all views share the same JavaScript VM. The `jit` option of `zg_runtime_config` therefore applies to the whole application and is independent of `allow_external_content`. The documentation recommends disabling it to those who load untrusted content (the optimizing parts of the JIT are the most frequent source of exploitable bugs in JSC, and it is what Apple's Lockdown Mode also turns off), at the cost of JS performance, but the choice remains with the developer.

## Update policy

- The engine follows main, where security fixes arrive first; at every critical WebKit advisory the pinned commit is updated immediately.
- Goal: a patch release of the engine within a few days of a critical advisory.
- The framework exposes the engine version and an optional release check, so applications know when they must be rebuilt.

## Fuzzing

The embedding layer, the bindings (value conversion between JSC and host), the custom schemes and the loader are new project code and must be fuzzed from the first version. WebCore and JSC are already fuzzed extensively upstream.
