# Provenance of port files

The embedding layer inherits the license of any code it is derived from
(ZeroG.md, "Licenze dei componenti"). Every file under `port/` and every patch
under `patches/` is listed here when it is created.

| File | Origin | License |
| --- | --- | --- |
| `include/zerog.h` | Original | BSD-2-Clause |
| `tools/webkit.py` | Original | BSD-2-Clause |
| `tools/build.py` | Original | BSD-2-Clause |
| `patches/0001-cmake-forwarding-headers-instead-of-symlinks-on-windows.patch` | Original, modifies `Source/cmake/WebKitMacros.cmake` (BSD) | BSD-2-Clause; upstream candidate |

Origins: **Original** (written for ZeroG), **WebKitLegacy** (derived from
`Source/WebKitLegacy`, BSD), **Ultralight WebCore** (derived from
github.com/ultralight-ux/WebCore, LGPL), **Upstream** (copied from another
WebKit port, license of the source file).
