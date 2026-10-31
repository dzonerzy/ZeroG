# Zig framework

The framework is the engine's first user and its main demo. The goal is feature parity with Wails, with a specific advantage: **bindings generated at comptime**, without runtime reflection, and with exact TypeScript types. The target is Zig 0.16.

## Generated bindings

The application defines a struct with `pub` methods. At comptime the framework inspects it with `@typeInfo` and generates:

- a dispatcher that decodes the arguments, calls the method and encodes the result, registered on the engine as a host call;
- a `bindings.d.ts` file and a `bindings.ts` client, produced by a `zig build` step that runs a small generator on the same module.

```zig
pub const App = struct {
    // Methods that run on a worker thread and return a Promise to JavaScript.
    pub const async_methods = .{"listFiles"};

    pub fn greet(self: *App, name: []const u8) ![]const u8 { ... }
    pub fn listFiles(self: *App, dir: []const u8) ![]FileEntry { ... }
};
```

In the frontend: `const msg: string = App.greet("Daniele")` (synchronous, without `await`) and `const files: FileEntry[] = await App.listFiles("/tmp")` (asynchronous). The generator emits `greet(name: string): string` and `listFiles(dir: string): Promise<FileEntry[]>` in `bindings.d.ts`; a name in `async_methods` that does not match a `pub` method is a compile error.

| Zig type | TypeScript type |
| --- | --- |
| Integers, floats | `number` (`bigint` for integers beyond 53 bits) |
| `bool` | `boolean` |
| `[]const u8` | `string` |
| Slices and arrays | `T[]` |
| Struct | `interface` |
| `enum` | union of string literals |
| `union(enum)` | discriminated union `{ tag: "...", value: ... }` |
| `?T` | `T \| null` |
| Error set | Rejected `Promise` (asynchronous calls) or JS exception (synchronous), with typed error code |

Every call receives an arena allocator, so methods do not manage the memory of the results: for synchronous calls it is freed when the callback returns, for asynchronous ones when the `Promise` has been resolved or rejected. Bindings are synchronous by default, because engine and application share the thread; the methods listed in `async_methods` run on a worker thread, the result is serialized and on the JS side they become `Promise`s.

## Parity with Wails

| Feature | Version 1 | Later |
| --- | --- | --- |
| Typed host → JS bindings | Yes | — |
| Events in both directions | Yes | — |
| Window: title, size, fullscreen, frameless, transparency, drag regions, click-through | Yes | Effects (blur, mica) |
| Multiple windows | — | Yes |
| Native dialogs (open, save, message) | Yes | — |
| Application menu and context menu | Yes | — |
| System tray | — | Yes |
| Clipboard | Yes | — |
| File drag and drop | Yes | — |
| Single instance | Yes | — |
| File association and URL scheme | — | Yes |
| Screen information | Yes | — |
| JS console in the terminal | Yes | — |
| Devtools (Web Inspector) | In the dev profile | — |

## CLI

- `init`: creates a project from a template (vanilla, React, Svelte, Vue) with `build.zig` and frontend configuration.
- `dev`: starts the frontend's dev server and the application with the `dev` engine profile, regenerates the bindings when the Zig code changes.
- `build`: builds the frontend, embeds the assets, generates the bindings and produces the static release binary.
- `package`: for macOS creates the `.app` bundle (mandatory on the platform); for Windows adds icon, manifest and version information to the executable.
