# sm-ext-otel

APM-style monitoring for a SourceMod server, exported over OpenTelemetry (OTLP/HTTP JSON) to any
backend that speaks OTLP (SigNoz, Grafana Tempo/Mimir, Datadog, ...) through a collector.

It answers "what is the server really doing?": which plugin callback eats the frame budget,
how long a server frame takes, which plugin leaks VM memory.

## What it measures

| Metric | Type | Attributes | Meaning |
| --- | --- | --- | --- |
| `sourcemod.callback.duration` | histogram (ms) | `sm.plugin`, `sm.function` | Inclusive duration of every SourcePawn callback (forwards, timers, hooks, commands, SQL callbacks...) |
| `sourcemod.callback.self_time` | sum (ms) | `sm.plugin`, `sm.function` | Same, minus the nested callbacks it triggered |
| `source.tick.cpu_time` | histogram (ms) | | CPU time of the main thread between two server frames, sleep excluded: the real cost of a tick (SourceMod timers, forwards, commands, engine) |
| `source.frame.duration` | histogram (ms) | | Wall time of `IServerGameDLL::GameFrame` only |
| `source.frame.interval` | histogram (ms) | | Time between two frames (jitter, missed ticks) |
| `source.frame.slow` | sum | | Ticks over `sm_otel_slow_ms` (CPU time or `GameFrame` time) |
| `source.players`, `source.entities`, `source.tickrate` | gauge | | |
| `sourcemod.plugins`, `sourcemod.plugin.memory` | gauge | `sm.plugin` | Running plugins, VM memory per plugin (leak hunting) |
| `process.cpu.time`, `process.memory.usage` | sum / gauge | `process.cpu.state` | srcds process (Linux) |
| `sm_otel.export.requests` | sum | `result` | Exporter health |

**Slow-frame traces**: when a tick exceeds `sm_otel_slow_ms`, one trace is exported with a
`ServerFrame` root span and one child span per callback that ran in it (nested as they were called),
so a spike can be attributed to a plugin and a function. The window runs from the previous
`GameFrame` to this one: SourceMod runs its timers and `OnGameFrame` in its own pre-frame hook, which
a plain `GameFrame` pre/post pair would miss. Traces are rate limited
(`sm_otel_slow_max_per_min`).

Resource attributes: `service.name`, `service.instance.id`, `deployment.environment.name`,
`server.port`, `game.mod`.

## Install

Drop `addons/sourcemod/extensions/otel.ext.2.css.so` (and `otel.autoload`) on the server. Point it
at a local OpenTelemetry Collector with an `otlp` HTTP receiver:

```
sm_otel_endpoint "http://127.0.0.1:4318"
```

Only plain `http://` is supported on purpose: let the local collector handle TLS, batching, retries
and authentication towards the backend. Exporting never runs on the game thread, and a dead
collector only increments `sm_otel.export.requests{result="failed"}`.

## ConVars (`cfg/sourcemod/extension.OTel.cfg`)

| ConVar | Default | |
| --- | --- | --- |
| `sm_otel_enable` | 1 | Master switch |
| `sm_otel_profile` | 1 | Time every SourcePawn callback (a few hundred ns per call) |
| `sm_otel_endpoint` | `http://127.0.0.1:4318` | Collector OTLP/HTTP base URL |
| `sm_otel_service_name` | `srcds` | `service.name` |
| `sm_otel_environment` | `production` | `deployment.environment.name` |
| `sm_otel_instance` | *(hostname:port)* | `service.instance.id` |
| `sm_otel_interval` | 15 | Seconds between metric exports |
| `sm_otel_slow_ms` | 10 | Slow frame threshold |
| `sm_otel_slow_max_per_min` | 12 | Slow-frame trace budget |
| `sm_otel_min_span_us` | 100 | Shortest callback kept in a trace |
| `sm_otel_max_series` | 500 | Busiest callbacks exported as metric series |

Console: `sm_otel_status` (state and exporter counters), `sm_otel_top [count] [self]` (busiest
callbacks since load, no backend needed).

## How it works

SourceMod 1.13.0.7427 keeps the SourcePawn VM profiler (`IProfilingTool`) private to its core, so an
extension cannot attach to it. Instead the extension hooks `IPluginFunction::Execute` and both
`Invoke` overloads with SourceHook (`Hook_VP`, i.e. on the vtable shared by every plugin function;
a plain per-instance hook would only see one function), and times
each call by `DebugName()` = `plugin.smx::Function`. The `Execute -> Invoke -> Invoke(args)` chain
is de-duplicated. Calls made from other threads are ignored; aggregation runs on the game thread
without locks. Switching profiling on/off only happens at the start of a frame, where no callback
can be running.

Limits: natives are not separate spans (their time is inside the callback that called them), and the
engine's own C++ time (networking, physics) is only visible as the difference between frame time and
the sum of callbacks.

## Build

```bash
tools/build-ext.sh sm-ext-otel        # from the srcdslab cockpit
```

Shared AMBuild scripts come from the `sm-ext-common` submodule; CI uses
`srcdslab/ci-workflows/.github/workflows/shared_build_release_am_extension.yml@v1`.
