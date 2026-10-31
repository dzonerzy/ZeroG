# App assets and schemes

## Your frontend, in your binary

Pack the frontend's build output at build time:

```sh
python tools/assets.py dist -o build/app_assets.c --c-symbol app_assets
```

and serve it:

```c
extern const unsigned char app_assets[];
extern const size_t app_assets_len;

zg_context_mount_assets(ctx, "app", app_assets, app_assets_len);
zg_view_load_url(view, "app://localhost/");
```

- Files are compressed with zlib where it pays off (formats that are already
  compressed, like images, fonts and media, are stored) and inflated by the
  engine on first use. The archive is used in place: keep it valid while the
  context lives, as static data is.
- `--exclude '*.map'` leaves files out; `--mime .wasm=application/wasm` sets a
  type; without `--c-symbol` the output is a `.zga` file to load at run time.
- The output is reproducible: files are sorted by path.

## What the app scheme serves

- `scheme://<any host>/<path>` with a MIME type from the file or its extension,
  range requests, `HEAD`, and `index.html` for directories. Use one host, such
  as `app://localhost/`: the host is part of the origin.
- Every scheme of a context is secure (`crypto.subtle` and other secure-context
  APIs work), CORS-enabled, and [trusted](security.md): its pages get the
  bindings.
- Single-page applications that route with the URL's path:
  `zg_context_set_fallback(ctx, "app", "/index.html")` answers paths without a
  file and without an extension with `index.html`.
- Files are write-once. `zg_context_add_file` adds one at run time, copied, or
  used in place with a release callback.

## Schemes answered by your code

```c
static void on_api(zg_scheme_request *req, void *user)
{
    const char *url = zg_scheme_request_url(req, NULL);
    /* answer now, or later from any thread */
    zg_scheme_request_add_header(req, "Cache-Control", "no-store");
    zg_scheme_request_respond(req, 200, "application/json", body, body_len);
}

zg_context_register_scheme(ctx, "api", on_api, NULL);
/* JS: await fetch("api://localhost/users") */
```

Answer each request exactly once, with `zg_scheme_request_respond` or
`zg_scheme_request_fail`. Built-in schemes (`http`, `https`, `file`, `data`,
`about`, `blob`, `ws`...) cannot be registered. A scheme serves either files or
a handler, not both.

## Fonts

`zg_context_register_font` adds a TrueType, OpenType, WOFF or WOFF2 font to
the context's pages under the family name inside it; a registered family wins
over a system font of the same name. Embedding your fonts makes text render the
same on every machine.
