# Licensing and relinking

ZeroG links WebKit statically into your application. WebKit's WebCore and
JavaScriptCore, and parts of WTF, are under the GNU LGPL (version 2 or later);
the rest of WebKit and most of the dependencies are under BSD-style licenses.
The full list is in the [design notes](../design/licensing-lgpl.md) and, file
by file for ZeroG's own derived code, in [PROVENANCE](../PROVENANCE.md).

!!! note
    This page explains what the LGPL asks of statically linked applications and
    the tools ZeroG provides for it. It is not legal advice.

## What the LGPL asks

Static linking is allowed for proprietary applications too, with one
condition: whoever receives the application must be able to **relink it with a
modified version of the LGPL parts**. In practice you ship, or offer:

1. The source of the engine version you linked (ZeroG's repository at that
   commit, which pins WebKit's revision and carries the patch series).
2. A **relink kit**: your application's object files and how to link them.
3. The license texts, and a notice that the application uses LGPL libraries.

Every other component in the engine (ICU, curl, BoringSSL, Skia, ANGLE, SDL,
the codecs...) has its own license, most of which ask for their notices to be
distributed with the binaries. They are all in
[`THIRD_PARTY_NOTICES.txt`](https://github.com/dzonerzy/ZeroG/blob/main/THIRD_PARTY_NOTICES.txt),
with the profiles that contain each component: ship it with your application
(in its installer, its about box or its documentation).
`python tools/notices.py` regenerates it from the WebKit checkout and the
dependency sysroot.

Applications under a license compatible with the LGPL (GPL, LGPL, MPL and
similar) already satisfy this by publishing their source.

## Making the relink kit

`tools/relink_kit.py` makes one from your build:

```sh
python tools/relink_kit.py create --name MyApp \
    --object build/main.obj --object build/app_assets.obj \
    --system-libs build/zerog-minimal/lib/zerog-engine.libs.txt \
    --link-arg /SUBSYSTEM:WINDOWS \
    --profile minimal --out MyApp-relink-kit
```

The kit holds:

- `objects/`: your application's object files, as linked;
- `kit.json`: the exact link (driver, arguments, system libraries) and the
  engine revision and profile;
- `README.md`: the steps, for whoever receives it;
- `relink.py`: the tool that relinks.

## Relinking

Whoever receives the kit builds the engine (modified or not) with the same
profile and relinks:

```sh
python tools/build.py all --profile minimal
python MyApp-relink-kit/relink.py relink MyApp-relink-kit \
    --bundle build/zerog-minimal/lib/zerog-engine.lib --out MyApp.exe
```

The relink needs neither your sources nor ZeroG's repository beyond the engine
itself. ZeroG's `linktest` builds the example app's kit and relinks it on every
build, so the path stays working.
