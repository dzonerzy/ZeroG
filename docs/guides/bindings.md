# JavaScript bindings

The page calls your C functions directly: JavaScriptCore's C API underneath,
no IPC, no serialization of every call. An empty synchronous call costs well
under a microsecond ([measurements](../project/measurements.md)).

## Functions

```c
/* JS: App.add(1, 2) === 3 */
static zg_value *add(zg_call *call, void *user)
{
    if (zg_call_argc(call) != 2 || zg_call_arg_type(call, 0) != ZG_TYPE_NUMBER)
        return zg_call_throw(call, "App.add takes two numbers");
    return zg_value_number(call, zg_call_arg_number(call, 0) + zg_call_arg_number(call, 1));
}

zg_view_bind(view, "App.add", add, NULL);
```

- The path is dotted from `window`; missing objects are created.
- Bindings are installed in every page of a [trusted origin](security.md), in
  the main frame only, before the page's own scripts run, and again in each new
  document. The origin is checked again on every call.
- Arguments convert as JavaScript does (`Boolean()`, `Number()`, `String()`,
  `JSON.stringify()`): `zg_call_arg_bool`, `_number`, `_int64`, `_string`,
  `_json`. They live until the callback returns.
- Return a value made on the call (`zg_value_number`, `_string`, `_json`,
  `_bool`, `_null`, `_int64` for BigInts) or NULL for `undefined`. To throw,
  return `zg_call_throw(call, message)`.

## Promises

For work that takes time, return a Promise and settle it later, from any
thread:

```c
static zg_value *read_file(zg_call *call, void *user)
{
    zg_promise *promise;
    zg_value *value = zg_call_promise(call, &promise);
    start_reading_on_my_thread(zg_call_arg_string(call, 0, NULL), promise);
    return value;  /* JS: const text = await App.readFile("notes.txt") */
}

/* Later, on any thread: */
zg_promise_resolve(promise, "\"file contents\"", len);   /* a JSON value */
zg_promise_reject(promise, "not found");
```

Each Promise must be settled exactly once; until then it keeps its page's
objects alive, even after the view is gone.

## Binary data

- `zg_call_arg_bytes` reads an `ArrayBuffer`, typed array or `DataView` in
  place, without a copy, and writes into it: a binding can fill a buffer the
  page passed.
- `zg_value_bytes` returns a `Uint8Array` with a copy; `zg_value_bytes_external`
  returns one over your memory, with a release callback once JavaScript is done
  with it.
- `zg_promise_resolve_bytes` resolves a Promise with bytes.

## Events and evaluation

- `zg_view_emit(view, "progress", json, len)` dispatches a `CustomEvent` on
  `window` with the JSON as its `detail`.
- `zg_view_evaluate` runs a script in the main frame and returns its result
  as JSON.
- `zg_view_add_script` runs a script in every page that gets the bindings,
  before the page's own: a framework's JavaScript runtime, for instance.

## Keeping JavaScript values

`zg_call_arg_ref` keeps an argument past the call: a callback to call later
with `zg_ref_call`, an object to hand back with `zg_value_ref`. A reference
keeps its page's objects alive until `zg_ref_release`.

## Classes

A class whose instances carry a native pointer:

```c
zg_class *file = zg_view_define_class(view, "App.File", file_constructor, file_finalize, NULL);
zg_class_add_method(file, "read", file_read, NULL);
zg_class_add_property(file, "size", file_size, NULL, NULL);  /* read-only */
```

The constructor runs for `new App.File(...)` and sets the pointer with
`zg_call_set_native`; methods and accessors get it with `zg_call_native`. The
finalizer runs once per pointer, after the instance is collected or when the
view goes. `instanceof`, `extends` and `zg_value_new_instance` (instances made
by native code) work as for built-in classes.
