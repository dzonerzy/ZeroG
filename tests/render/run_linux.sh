#!/bin/sh
# Runs the render tests in the Linux builder image (README, "Building"), on
# whatever EGL driver the container has (Mesa llvmpipe by default; see the README
# for the GPU through WSL 2). Every EGL display path and both window systems are
# covered: Xvfb for X11, Weston's headless backend for Wayland.
#
#   docker run --rm -v zerog-linux-build:/build zerog-linux-builder \
#       sh /src/tests/render/run_linux.sh [build dir, default /build/zerog-minimal]

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
run "input" "$bin/ZeroGInput"
run "resource loader" "$bin/ZeroGLoader"
run "window on X11" xvfb-run -a "$bin/ZeroGWindow"
run "C API" xvfb-run -a "$bin/ZeroGAPITest"
run "C API without a display" "$bin/ZeroGAPITest" --no-window
run "value conversions" "$bin/ZeroGValuesTest"
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
