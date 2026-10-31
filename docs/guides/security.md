# Trust and security

ZeroG runs pages in your process, with your privileges, and lets them call your
native code. Which pages may do that is the most important decision an
application makes; the defaults are strict.

## Trusted origins

A trusted origin may be navigated to and gets the bindings. By default the
trusted origins are only the application's own:

- the schemes mounted with `zg_context_mount_assets` or registered with
  `zg_context_register_scheme`;
- in the `dev` and `debug` profiles, `zg_context_config.dev_server_url` (your
  frontend's development server, such as `http://localhost:5173`). The
  `minimal` and `standard` profiles ignore it, so it cannot ship to production
  by mistake.

Anything else is decided by `zg_context_config.trust`, a callback asked for
navigation and for bindings separately; without it, the answer is no.

Bindings go to the main frame only, and the origin is checked again on every
call: a page that navigated elsewhere, or an iframe, cannot reach them.

## External content

By default a view only loads trusted origins: a navigation elsewhere is
blocked and handed to the `open_external` callback (open it in the system's
browser, for instance), unless `navigation_blocked` allows it.

`zg_context_config.allow_external_content` lets pages load content from other
origins (navigation, iframes). Untrusted origins never get the bindings, but
ZeroG adds no sandbox beyond the web platform's own: the developer takes
responsibility for what is loaded.

## The JIT

`zg_runtime_config.jit` turns JavaScriptCore's JIT compilers off, for
applications that only run their own code and want less attack surface, or for
platforms that forbid executable memory. The `minimal` profile has no
WebAssembly and no FTL tier.

## Certificates

https and wss trust the system's root certificates (the Windows store, the
distribution's CA bundle on Linux) and those added with
`zg_runtime_add_ca_certificates` (a private CA, a development server's). An
untrusted certificate fails the load with "certificate not trusted" and the
reason in `zg_last_error()`.

## What ZeroG does not isolate

There is one process. A crash in WebCore or JavaScriptCore ends the
application, and a page that compromises the engine has your process. Keep
untrusted content out of views that matter, or out of the application.
