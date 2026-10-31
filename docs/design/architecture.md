# Architecture

The application sees only the C API; below it are the embedding layer and the platform layer written by the project, with upstream WebKit in between. Everything lives in a single process.

<!-- architecture diagram: architecture · one process, six layers -->

Calls go down from the application to presentation. JS bindings take the reverse path without IPC: JavaScriptCore directly calls the native code registered by the framework, on the same main thread.
