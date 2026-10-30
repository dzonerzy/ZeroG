#!/bin/sh
# Runs the render tests in the Linux builder image (README, "Building"), on
# whatever EGL driver the container has (Mesa llvmpipe by default; see the README
# for the GPU through WSL 2). Every EGL display path and both window systems are
# covered: Xvfb for X11, Weston's headless backend for Wayland. The accessibility
# test runs under dbus-run-session, with at-spi2-core's bus and registry.
#
#   docker run --rm --device /dev/dri -v zerog-linux-build:/build zerog-linux-builder \
#       sh /src/tests/render/run_linux.sh [build dir, default /build/zerog-minimal]
#
# (--device /dev/dri for the dma-buf test; without it that test is skipped.)

bin=${1:-/build/zerog-minimal}/bin
failed=0
run() {
    name=$1
    shift
    if "$@" > /tmp/test.log 2>&1; then
        echo "PASS $name"
    else
        echo "FAIL $name"
        grep -v "^ok" /tmp/test.log | head -20
        failed=1
    fi
}

cd /tmp
run "first render" "$bin/ZeroGFirstRender" --gpu first_render.png
for platform in surfaceless device; do
    run "GPU on the EGL $platform platform" env ZEROG_EGL_PLATFORM=$platform "$bin/ZeroGFirstRender" --gpu first_render.png
done
run "default EGL display (X11)" env ZEROG_EGL_PLATFORM=default xvfb-run -a "$bin/ZeroGFirstRender" --gpu first_render.png
run "frame loop" "$bin/ZeroGFrameLoop"
run "texture mode" "$bin/ZeroGTexture"
# dma-bufs need a GPU that exports them, GBM, or a DRM device for dumb buffers
# (llvmpipe exports none): docker run --device /dev/dri gives the VM's.
if [ -e /dev/dri ]; then
    run "texture mode with dma-bufs (Vulkan)" "$bin/ZeroGTextureDMABuf"
else
    echo "SKIP texture mode with dma-bufs: no /dev/dri (docker run --device /dev/dri)"
fi
run "GPU compositing" "$bin/ZeroGCompositing"
run "input" "$bin/ZeroGInput"
run "resource loader" "$bin/ZeroGLoader"
run "window on X11" xvfb-run -a "$bin/ZeroGWindow"
run "C API" xvfb-run -a "$bin/ZeroGAPITest"
run "C API without a display" "$bin/ZeroGAPITest" --no-window
run "value conversions" "$bin/ZeroGValuesTest"
run "scripts, bytes, references and classes" "$bin/ZeroGBindingsTest"
run "localStorage, sessionStorage and IndexedDB" "$bin/ZeroGStorageTest"
run "clipboard, file chooser, drag and drop" "$bin/ZeroGHostTest"
run "Web Inspector (dev and debug builds)" "$bin/ZeroGInspector"
run "create and destroy cycles" xvfb-run -a "$bin/ZeroGLifecycleTest"
# A session bus of its own; the AT-SPI bus and registry start on demand.
run "accessibility (AT-SPI)" dbus-run-session -- xvfb-run -a timeout 120 "$bin/ZeroGAccessibility"
for target in assets requests values; do
    run "fuzz corpus: $target" "$bin/ZeroGFuzz$(echo $target | sed 's/./\U&/')" /src/tests/fuzz/corpus/$target/*
done
run "hello example" xvfb-run -a "$bin/ZeroGHello" --smoke
run "binding latency" "$bin/ZeroGBindingBench"
grep "us per" /tmp/test.log

export XDG_RUNTIME_DIR=/tmp/zerog-xdg
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
weston --backend=headless --renderer=gl --socket=zerog-wayland > /tmp/weston.log 2>&1 &
weston=$!
sleep 3
run "window on Wayland" env WAYLAND_DISPLAY=zerog-wayland SDL_VIDEO_DRIVER=wayland timeout 120 "$bin/ZeroGWindow"
kill $weston 2>/dev/null

exit $failed
