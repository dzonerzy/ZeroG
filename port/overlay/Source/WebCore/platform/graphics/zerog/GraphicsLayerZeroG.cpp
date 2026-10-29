// Compositing version 1 (ZeroG.md, "Compositing"): accelerated compositing is off
// and WebCore paints the whole page into one Skia surface. WebCore still creates
// graphics layers for page overlays and similar, so they exist but are never
// composited. Version 2 replaces this with a real compositor.

#include "config.h"
#include "GraphicsLayer.h"

#include "GraphicsLayerFactory.h"

namespace WebCore {

namespace {

class GraphicsLayerZeroG final : public GraphicsLayer {
public:
    GraphicsLayerZeroG(Type layerType, GraphicsLayerClient& client)
        : GraphicsLayer(layerType, client)
    {
    }

private:
    void setNeedsDisplay() final { }
    void setNeedsDisplayInRect(const FloatRect&, ShouldClipToLayer) final { }
};

} // namespace

Ref<GraphicsLayer> GraphicsLayer::create(GraphicsLayerFactory* factory, GraphicsLayerClient& client, Type layerType)
{
    if (factory)
        return factory->createGraphicsLayer(layerType, client);
    return adoptRef(*new GraphicsLayerZeroG(layerType, client));
}

} // namespace WebCore
