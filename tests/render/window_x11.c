/*
 * The X server's side of a window for tests/render/window.cpp, apart so that
 * Xlib's macros (None, Bool, Status...) stay out of WebCore's headers.
 */

#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

/* 1 if (x, y), in the window's pixels, is in its input shape: where clicks go
 * to the window rather than through it. -1 without the shape extension. */
int zerog_test_x11_input_shape_contains(void* display, unsigned long window, int x, int y)
{
    int eventBase, errorBase, count = 0, ordering, inside = 0;
    if (!XShapeQueryExtension((Display*)display, &eventBase, &errorBase))
        return -1;
    XRectangle* rects = XShapeGetRectangles((Display*)display, (Window)window, ShapeInput, &count, &ordering);
    for (int i = 0; i < count && !inside; ++i)
        inside = x >= rects[i].x && x < rects[i].x + rects[i].width && y >= rects[i].y && y < rects[i].y + rects[i].height;
    if (rects)
        XFree(rects);
    return inside;
}
