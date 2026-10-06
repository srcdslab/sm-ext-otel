# sm-ext-otel

SourceMod C++ extension (AMBuild, `sm-ext-common` submodule) exporting APM data over OpenTelemetry OTLP/HTTP JSON.

- `src/profiler.*`: times every plugin callback (`IPluginFunction::Execute/Invoke` hooks), main thread only, no locks.
- `src/exporter.*`: background HTTP POST thread, plain `http://` to a local collector, never blocks the game thread.
- `src/otlp.h`: hand-written OTLP JSON encoder (metrics, traces).
- `src/extension.cpp`: GameFrame hooks, slow-tick traces, metric flush, ConVars, `sm_otel_status` / `sm_otel_top`.

Build: `tools/build-ext.sh sm-ext-otel` from the cockpit (SM 1.13.0.7427 pin). Runtime-test on the prod-identical
image (x86, `.build/otel-test`). The VM profiler (`IProfilingTool`) is private in SM 7427: do not rely on it.
SourceHook hooks on `IPluginFunction` must be `Hook_VP` (shared vtable). Keep source files LF.
