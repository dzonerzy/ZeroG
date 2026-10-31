# Web Inspector

WebKit's Web Inspector is in the `dev` and `debug` profiles: elements, styles,
console, sources and debugger, network, timelines, storage. The `minimal` and
`standard` profiles do not contain it, so it cannot ship by mistake.

## In a window

```c
zg_view_show_inspector(view);  /* or F12 / Ctrl+Shift+I in window mode */
```

The inspector opens in a window of its own (or comes to the front), for any
view, window or texture mode. Its windows run in `zg_runtime_run` (and
`zg_runtime_update`) and do not count as the application's: closing them ends
nothing. In the other profiles the call fails with `ZG_ERROR_UNSUPPORTED`.

## Remotely

```c
zg_runtime_config config = { sizeof config };
config.inspector_port = 9222;
```

starts the remote inspector's server on 127.0.0.1, so a WebKit frontend
elsewhere can inspect every view; reach it from another machine through a
tunnel. A warning goes to the log if the port cannot be opened.

## With a development server

In the dev profile, `zg_context_config.dev_server_url` trusts your frontend's
development server (Vite, webpack...), so the app can load it with hot reload
and keep its bindings: see [Trust and security](security.md).
