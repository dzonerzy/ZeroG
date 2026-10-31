# Measurable goals

Every metric is measured in CI at every commit, on the same example app, and compared with WebView2 and WebKitGTK. The numeric goals are set after phase 1, when a first real measurement exists.

| Metric | How it is measured | Goal |
| --- | --- | --- |
| Binary size | Hello world in the minimal profile, after symbol stripping ([`tools/size.py`](https://github.com/dzonerzy/ZeroG/blob/main/tools/size.py)) | Measured: 93.1 MB on Windows, 90.2 MB on Linux ([Binary size](text-fonts.md#binary-size)); no more than that |
| Idle memory | RSS with an empty view loaded | To be set after phase 1 |
| Memory with example app | RSS with the React demo open | To be set after phase 1 |
| Startup time | From process start to the first frame shown | To be set after phase 1 |
| Smoothness | Frame time of a reference CSS animation | To be set after phase 1 |
| Binding latency | Empty synchronous and asynchronous call, in microseconds | To be set after phase 2 |

An increase beyond a threshold compared with the previous release makes the CI fail, so size and memory regressions are seen immediately.
