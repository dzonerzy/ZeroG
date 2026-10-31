# Storage and networking

## Where data lives

A context keeps its state in a data directory: `zg_context_config.data_dir`,
or by default one named after `app_id` (`%LOCALAPPDATA%\<app_id>` on Windows,
`$XDG_DATA_HOME/<app_id>` on Linux). With neither, or with `ephemeral` set,
everything lives in memory and nothing is written to disk. Contexts on the same
directory share what is in it.

| What | Where in the data directory | Profiles |
| --- | --- | --- |
| Cookies | `Cookies.sqlite` | all |
| `localStorage` | `LocalStorage/` | all |
| IndexedDB | `IndexedDB/` | all |
| HTTP disk cache | `HTTPCache/` | all |
| Cache API (`caches`) | `CacheStorage/` | standard, dev, debug |
| Service worker registrations | `ServiceWorkers/` | standard, dev, debug |

`sessionStorage` is per view and never written to disk.

## Network

http and https go through curl with BoringSSL on every OS, with HTTP/2,
cookies, redirects, CORS and the HTTP disk cache:

- `zg_context_config.http_cache_limit` sizes the cache (0 = 64 MB, negative =
  none). Responses are kept and revalidated as HTTP caching says (`max-age`,
  `Expires`, `no-store`, `no-cache`, `Vary`, `ETag`, `Last-Modified`).
- WebSockets (`ws://`, `wss://`) and `fetch`/`XMLHttpRequest`, synchronous XHR
  included, work as in a browser.
- Certificates: the system's roots plus those added with
  `zg_runtime_add_ca_certificates`.
- HTTP authentication asks the `auth_challenge` callback; credentials given are
  kept in memory for the context's lifetime. Without the callback every
  challenge is cancelled and the 401 page shown.

## Downloads

A navigation to an attachment, or an `<a download>` link (`blob:` and `data:`
URLs included), asks `download_started`; give it a destination with
`zg_download_set_destination` or it is refused. `download_progress` and
`download_finished` follow; a failed or cancelled download's partial file is
removed.

## Service workers

In the standard, dev and debug profiles, pages served over http and https can
register service workers: install and activate, fetch events for navigations
and subresources, messages, the Cache API inside them, navigation preload, and
script updates. Push, notifications and background fetch are not supported.
The minimal profile has no `navigator.serviceWorker`.
