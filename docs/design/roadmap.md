# Roadmap

Development starts from the port itself, on Linux and Windows together. The critical decision point is phase 1: if the embedding layer cannot get WebCore to render without WebKit2 with reasonable effort, the plan must be revised before investing in the rest.

<!-- roadmap diagram: roadmap · 7 phases, one gate per phase -->

Each phase closes only when its gate is passed. Phase 5 can start as soon as the C API of phase 2 is stable, in parallel with phases 3 and 4. Durations are not estimated here: they must be set after phase 0, when the real build and iteration times on WebKit are known.
