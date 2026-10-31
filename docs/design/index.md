# ZeroG — Technical documentation

Oct 28, 2026 · @Daniele

## Pages

- [Overview](overview.md): goals, non-goals and target use cases of the engine.
- [Context](context.md): comparison with the existing alternatives (Ultralight, WPE, CEF, Servo, system webviews, Blitz).
- [Architecture](architecture.md): the layers of the single process, from the C API down to presentation.
- [Engine port](engine-port.md): the project's own WebKit port, its layers, references, CMake port, Skia, JavaScriptCore and upstream revision.
- [Process model](process-model.md): threads, integration with the application loop and the consequences of single-process.
- [Coexisting with the host application](host-coexistence.md): allocator, signals, symbols, runtime singleton, JIT policies and engine threads.
- [Graphics and presentation](graphics-presentation.md): window, texture and CPU modes, GPU backends, compositing, frame cycle and render thread handoff.
- [Text and fonts](text-fonts.md): font stack, embedded fonts, ICU data and binary size.
- [Networking, storage and resources](networking-storage-resources.md): curl and BoringSSL, certificates, caches, service workers, `app://` scheme, dev mode and storage.
- [Input, IME, clipboard, accessibility](input-ime-clipboard-accessibility.md): events, IME, clipboard, drag and drop, dialogs, context menus, frameless and transparent windows, accessibility.
- [Public C API](c-api.md): principles, objects, loop, direct JavaScript bindings, usage example and diagnostics.
- [Build system and static linking](build-system-static-linking.md): build structure, media engine, build profiles, times and cache.
- [Dependencies: what to remove, keep, replace](dependencies.md): criteria and decisions for every dependency, features turned off, what stays dynamic.
- [Licenses and LGPL compliance](licensing-lgpl.md): component licenses, relink kit and compliance measures.
- [Security](security.md): trust model, attack surface, JIT, update policy and fuzzing.
- [Zig framework](zig-framework.md): comptime-generated bindings, parity with Wails and CLI.
- [Measurable goals](measurable-goals.md): metrics measured in CI and their targets.
- [Roadmap](roadmap.md): phases and gates of development.
- [Testing, CI and upstream](testing-ci-upstream.md): test levels, CI, symbols and crashes, upstream strategy.
- [Risks and open questions](risks-open-questions.md): risks with mitigations and the list of open and decided questions.
- [Sources](sources.md): external references cited in the document.
