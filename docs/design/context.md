# Context

No current alternative offers at the same time a complete web engine, an open source license, identical behavior on three OSes and distribution as a single binary. This is the project's space.

| Alternative | Engine | JS | Distribution | License | Main limitation |
| --- | --- | --- | --- | --- | --- |
| [Ultralight](https://github.com/ultralight-ux/Ultralight) | Fork of WebCore + own renderer | Yes (JavaScriptCore) | DLL; static linking not allowed with the free license | Proprietary core, WebCore LGPL/BSD | Commercial license for paid use |
| [WPE WebKit](https://wpewebkit.org/) | Upstream WebKit | Yes | Dynamic libraries, multi-process | LGPL/BSD | Linux only |
| CEF | Chromium | Yes | Dynamic libraries, hundreds of MB | BSD | Size and complexity |
| [Servo](https://servo.org/blog/2026/04/13/servo-0.1.0-release/) | Servo (Rust) | Yes | Rust crate, v0.1.0 since April 2026 | MPL 2.0 | Web compatibility still lower than WebKit and Chromium |
| System webview (WebView2, WKWebView, WebKitGTK) | Different per OS | Yes | System dependency | Varies | Different behavior per OS, WebKitGTK fragmented on Linux |
| [Blitz](https://github.com/DioxusLabs/blitz) | Modular HTML/CSS engine | No | Rust crate | MIT/Apache | Pre-alpha, no JavaScript by choice |

The engine's competitive advantage is the combination of three things: WebKit compatibility, a C API for any language and single-file distribution. Servo is the most direct open source competitor; the comparison must be redone at each of its LTS releases.
