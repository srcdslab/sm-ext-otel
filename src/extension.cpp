#include "extension.h"
#include "convarhelper.h"

#include <sourcehook.h>
#include <eiface.h>
#include <algorithm>
#include <vector>

#ifdef _WIN32
	#include <windows.h>
#else
	#include <unistd.h>
	#ifdef PLATFORM_LINUX
		#include <sys/resource.h>
	#endif
#endif

using namespace otel;

OTelExt g_OTel;
SMEXT_LINK(&g_OTel);

SH_DECL_HOOK1_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool);

// IPluginFunction::Execute / Invoke are the public entry points into plugin code.
// Invoke(args) is overloaded, so these are manual hooks reconfigured with the
// vtable index of the SM headers we are built against. They are installed as VP
// (vtable) hooks: SourceHook's default hooks only cover the one instance given.
SH_DECL_MANUALHOOK1(IPF_Execute, 0, 0, 0, int, cell_t *);
SH_DECL_MANUALHOOK1(IPF_Invoke, 0, 0, 0, bool, cell_t *);
SH_DECL_MANUALHOOK2(IPF_InvokeArgs, 0, 0, 0, bool, const sp::CallArgs &, cell_t *);

CGlobalVars *gpGlobals = nullptr;
ICvar *g_pCVar = nullptr;

ConVar *g_cvEnable = CreateConVar("sm_otel_enable", "1", FCVAR_NOTIFY, "Master switch: frame metrics, profiling and export.", true, 0.0f, true, 1.0f);
ConVar *g_cvProfile = CreateConVar("sm_otel_profile", "1", FCVAR_NOTIFY, "Time every SourcePawn callback (per plugin / function). Costs a few hundred ns per call.", true, 0.0f, true, 1.0f);
ConVar *g_cvEndpoint = CreateConVar("sm_otel_endpoint", "http://127.0.0.1:4318", FCVAR_NONE, "OTLP/HTTP endpoint of the local collector (plain http:// only).");
ConVar *g_cvService = CreateConVar("sm_otel_service_name", "srcds", FCVAR_NOTIFY, "OpenTelemetry service.name.");
ConVar *g_cvEnvironment = CreateConVar("sm_otel_environment", "production", FCVAR_NOTIFY, "OpenTelemetry deployment.environment.name.");
ConVar *g_cvInstance = CreateConVar("sm_otel_instance", "", FCVAR_NOTIFY, "OpenTelemetry service.instance.id. Empty = <machine hostname>:<hostport>.");
ConVar *g_cvInterval = CreateConVar("sm_otel_interval", "15", FCVAR_NOTIFY, "Seconds between metric exports.", true, 5.0f, true, 300.0f);
ConVar *g_cvSlowMs = CreateConVar("sm_otel_slow_ms", "10", FCVAR_NOTIFY, "A server frame longer than this (ms) counts as slow and exports a trace of the callbacks it ran.", true, 1.0f, true, 1000.0f);
ConVar *g_cvSlowMaxPerMin = CreateConVar("sm_otel_slow_max_per_min", "12", FCVAR_NOTIFY, "Maximum slow-frame traces exported per minute.", true, 1.0f, true, 600.0f);
ConVar *g_cvMinSpanUs = CreateConVar("sm_otel_min_span_us", "100", FCVAR_NOTIFY, "Callbacks shorter than this (microseconds) are left out of slow-frame traces.", true, 0.0f, true, 100000.0f);
ConVar *g_cvMaxSeries = CreateConVar("sm_otel_max_series", "500", FCVAR_NOTIFY, "Maximum callbacks (busiest first) exported as metric series.", true, 10.0f, true, 4096.0f);

static ConVar *g_pHostname = nullptr;
static ConVar *g_pHostport = nullptr;

static void CmdStatus(const CCommand &args) { g_OTel.PrintStatus(); }
static void CmdTop(const CCommand &args)
{
	int count = args.ArgC() > 1 ? atoi(args.Arg(1)) : 20;
	bool bySelf = args.ArgC() > 2 && strcmp(args.Arg(2), "self") == 0;
	g_OTel.PrintTop(count > 0 ? count : 20, bySelf);
}

static ConCommand g_cmdStatus("sm_otel_status", CmdStatus, "Show sm-ext-otel state and exporter counters.");
static ConCommand g_cmdTop("sm_otel_top", CmdTop, "sm_otel_top [count] [self] - busiest SourcePawn callbacks since the extension loaded.");

bool OTelExt::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_ANY(GetServerFactory, gamedll, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);

	gpGlobals = ismm->GetCGlobals();
	ConVar_Register(0, this);

	g_pHostname = g_pCVar->FindVar("hostname");
	g_pHostport = g_pCVar->FindVar("hostport");
	return true;
}

bool OTelExt::RegisterConCommandBase(ConCommandBase *pVar)
{
	return META_REGCVAR(pVar);
}

bool OTelExt::SDK_OnLoad(char *error, size_t maxlen, bool late)
{
	m_Profiler.BindThread();
	m_StartWallNs = WallNs();
	m_NextFlushNs = NowNs() + (uint64_t)g_cvInterval->GetInt() * 1000000000ULL;

	if (!m_Exporter.Start()) {
		snprintf(error, maxlen, "Failed to start the exporter thread");
		return false;
	}

	SourceHook::MemFuncInfo mfi = {true, -1, 0, 0};
	SourceHook::GetFuncInfo(static_cast<int (SourcePawn::IPluginFunction::*)(cell_t *)>(&SourcePawn::IPluginFunction::Execute), mfi);
	SH_MANUALHOOK_RECONFIGURE(IPF_Execute, mfi.vtblindex, mfi.vtbloffs, mfi.thisptroffs);
	m_VtblIdx[0] = mfi.vtblindex;
	SourceHook::GetFuncInfo(static_cast<bool (SourcePawn::IPluginFunction::*)(cell_t *)>(&SourcePawn::IPluginFunction::Invoke), mfi);
	SH_MANUALHOOK_RECONFIGURE(IPF_Invoke, mfi.vtblindex, mfi.vtbloffs, mfi.thisptroffs);
	m_VtblIdx[1] = mfi.vtblindex;
	SourceHook::GetFuncInfo(static_cast<bool (SourcePawn::IPluginFunction::*)(const sp::CallArgs &, cell_t *)>(&SourcePawn::IPluginFunction::Invoke), mfi);
	SH_MANUALHOOK_RECONFIGURE(IPF_InvokeArgs, mfi.vtblindex, mfi.vtbloffs, mfi.thisptroffs);
	m_VtblIdx[2] = mfi.vtblindex;

	SH_ADD_HOOK(IServerGameDLL, GameFrame, gamedll, SH_MEMBER(this, &OTelExt::Hook_GameFramePre), false);
	SH_ADD_HOOK(IServerGameDLL, GameFrame, gamedll, SH_MEMBER(this, &OTelExt::Hook_GameFramePost), true);

	plsys->AddPluginsListener(this);

	AutoExecConfig(g_pCVar, true);
	return true;
}

void OTelExt::SDK_OnAllLoaded()
{
	// Late load: plugins are already running, grab a function to hook right away.
	SourceMod::IPluginIterator *iter = plsys->GetPluginIterator();
	for (; iter->MorePlugins() && !m_HookedFn; iter->NextPlugin()) {
		SourceMod::IPlugin *pl = iter->GetPlugin();
		if (pl->GetStatus() == SourceMod::Plugin_Running)
			TryInstallHooks(pl->GetRuntime());
	}
	iter->Release();
}

void OTelExt::SDK_OnUnload()
{
	SH_REMOVE_HOOK(IServerGameDLL, GameFrame, gamedll, SH_MEMBER(this, &OTelExt::Hook_GameFramePre), false);
	SH_REMOVE_HOOK(IServerGameDLL, GameFrame, gamedll, SH_MEMBER(this, &OTelExt::Hook_GameFramePost), true);

	plsys->RemovePluginsListener(this);
	RemoveHooks();
	m_Exporter.Stop();

	ConVar_Unregister();
}

bool OTelExt::TryInstallHooks(SourcePawn::IPluginRuntime *runtime)
{
	if (m_HookedFn || !runtime)
		return m_HookedFn != nullptr;

	// Public function ids are (index << 1) | 1; the first out-of-range id yields null.
	SourcePawn::IPluginFunction *fn = nullptr;
	for (unsigned i = 0; i < 64 && !fn; i++)
		fn = runtime->GetFunctionById((i << 1) | 1);
	if (!fn)
		return false;

	// Hook_VP: patch the shared vtable so every plugin function is covered, not just `fn`.
	SH_ADD_MANUALVPHOOK(IPF_Execute, fn, SH_MEMBER(this, &OTelExt::Hook_ExecutePre), false);
	SH_ADD_MANUALVPHOOK(IPF_Execute, fn, SH_MEMBER(this, &OTelExt::Hook_ExecutePost), true);
	SH_ADD_MANUALVPHOOK(IPF_Invoke, fn, SH_MEMBER(this, &OTelExt::Hook_InvokePre), false);
	SH_ADD_MANUALVPHOOK(IPF_Invoke, fn, SH_MEMBER(this, &OTelExt::Hook_InvokePost), true);
	SH_ADD_MANUALVPHOOK(IPF_InvokeArgs, fn, SH_MEMBER(this, &OTelExt::Hook_InvokeArgsPre), false);
	SH_ADD_MANUALVPHOOK(IPF_InvokeArgs, fn, SH_MEMBER(this, &OTelExt::Hook_InvokeArgsPost), true);
	m_HookedFn = fn;
	return true;
}

void OTelExt::RemoveHooks()
{
	if (!m_HookedFn)
		return;

	SourcePawn::IPluginFunction *fn = m_HookedFn;
	m_HookedFn = nullptr;
	m_ProfileOn = false;

	SH_REMOVE_MANUALHOOK(IPF_Execute, fn, SH_MEMBER(this, &OTelExt::Hook_ExecutePre), false);
	SH_REMOVE_MANUALHOOK(IPF_Execute, fn, SH_MEMBER(this, &OTelExt::Hook_ExecutePost), true);
	SH_REMOVE_MANUALHOOK(IPF_Invoke, fn, SH_MEMBER(this, &OTelExt::Hook_InvokePre), false);
	SH_REMOVE_MANUALHOOK(IPF_Invoke, fn, SH_MEMBER(this, &OTelExt::Hook_InvokePost), true);
	SH_REMOVE_MANUALHOOK(IPF_InvokeArgs, fn, SH_MEMBER(this, &OTelExt::Hook_InvokeArgsPre), false);
	SH_REMOVE_MANUALHOOK(IPF_InvokeArgs, fn, SH_MEMBER(this, &OTelExt::Hook_InvokeArgsPost), true);
	m_Profiler.ResetStack();
}

void OTelExt::OnPluginLoaded(SourceMod::IPlugin *plugin)
{
	if (!m_HookedFn)
		TryInstallHooks(plugin->GetRuntime());
}

void OTelExt::OnPluginUnloaded(SourceMod::IPlugin *plugin)
{
	m_Profiler.ClearFnCache();
}

void OTelExt::OnPluginDestroyed(SourceMod::IPlugin *plugin)
{
	m_Profiler.ClearFnCache();
}

// All six handlers do the same thing; the pre/post pair brackets one plugin call.
int OTelExt::Hook_ExecutePre(cell_t *result)
{
	m_HookHits[0]++;
	if (m_ProfileOn)
		m_Profiler.Enter(META_IFACEPTR(SourcePawn::IPluginFunction));
	RETURN_META_VALUE(MRES_IGNORED, 0);
}

int OTelExt::Hook_ExecutePost(cell_t *result)
{
	if (m_ProfileOn)
		m_Profiler.Leave();
	RETURN_META_VALUE(MRES_IGNORED, 0);
}

bool OTelExt::Hook_InvokePre(cell_t *result)
{
	m_HookHits[1]++;
	if (m_ProfileOn)
		m_Profiler.Enter(META_IFACEPTR(SourcePawn::IPluginFunction));
	RETURN_META_VALUE(MRES_IGNORED, false);
}

bool OTelExt::Hook_InvokePost(cell_t *result)
{
	if (m_ProfileOn)
		m_Profiler.Leave();
	RETURN_META_VALUE(MRES_IGNORED, false);
}

bool OTelExt::Hook_InvokeArgsPre(const sp::CallArgs &args, cell_t *result)
{
	m_HookHits[2]++;
	if (m_ProfileOn)
		m_Profiler.Enter(META_IFACEPTR(SourcePawn::IPluginFunction));
	RETURN_META_VALUE(MRES_IGNORED, false);
}

bool OTelExt::Hook_InvokeArgsPost(const sp::CallArgs &args, cell_t *result)
{
	if (m_ProfileOn)
		m_Profiler.Leave();
	RETURN_META_VALUE(MRES_IGNORED, false);
}

void OTelExt::Hook_GameFramePre(bool simulating)
{
	// Frame start is the only place scopes cannot be open, so the switch is safe here.
	m_ProfileOn = g_cvEnable->GetBool() && g_cvProfile->GetBool() && m_HookedFn;
	if (!g_cvEnable->GetBool()) {
		m_Profiler.EndFrame();
		RETURN_META(MRES_IGNORED);
	}

	m_FrameStartNs = NowNs();

	// Intervals above a second are the server waking up from hibernation, not a tick.
	if (m_PrevFrameStartNs) {
		uint64_t interval = m_FrameStartNs - m_PrevFrameStartNs;
		if (interval < 1000000000ULL)
			m_IntervalStat.Record(interval, 0);
	}
	m_PrevFrameStartNs = m_FrameStartNs;

	// The span window is rolling (previous GameFrame post -> this post) so it also covers
	// what SourceMod itself runs before our pre hook: timers, OnGameFrame, queued commands.
	if (m_ProfileOn && !m_Profiler.FrameActive())
		m_Profiler.BeginFrame();
	else if (!m_ProfileOn)
		m_Profiler.EndFrame();

	RETURN_META(MRES_IGNORED);
}

void OTelExt::Hook_GameFramePost(bool simulating)
{
	if (!g_cvEnable->GetBool() || !m_FrameStartNs) {
		RETURN_META(MRES_IGNORED);
	}

	uint64_t end = NowNs();
	uint64_t gameFrame = end - m_FrameStartNs;
	m_FrameStat.Record(gameFrame, gameFrame);

	// Busy time of the main thread since the previous frame: sleeping excluded, SourceMod's
	// own pre-frame work (timers, forwards) and the engine's work around GameFrame included.
	uint64_t cpu = ThreadCpuNs();
	uint64_t tickCpu = (m_PrevPostCpuNs && cpu > m_PrevPostCpuNs) ? cpu - m_PrevPostCpuNs : 0;
	m_PrevPostCpuNs = cpu;
	if (tickCpu)
		m_TickCpuStat.Record(tickCpu, tickCpu);

	uint64_t slowNs = (uint64_t)g_cvSlowMs->GetInt() * 1000000ULL;
	if (std::max(gameFrame, tickCpu) >= slowNs) {
		m_SlowFrames++;
		EmitSlowTrace(end, gameFrame, tickCpu);
	}

	m_Profiler.EndFrame();
	if (m_ProfileOn)
		m_Profiler.BeginFrame();

	if (end >= m_NextFlushNs)
		Flush(end);

	RETURN_META(MRES_IGNORED);
}

std::string OTelExt::Endpoint()
{
	return g_cvEndpoint->GetString();
}

Attrs OTelExt::BuildResource()
{
	const char *service = g_cvService->GetString();
	int port = g_pHostport ? g_pHostport->GetInt() : 0;

	std::string instance = g_cvInstance->GetString();
	if (instance.empty()) {
		char host[128] = "srcds";
#ifdef _WIN32
		DWORD hostLen = sizeof(host) - 1;
		GetComputerNameA(host, &hostLen);
#else
		gethostname(host, sizeof(host) - 1);
#endif
		host[sizeof(host) - 1] = '\0';
		char buf[160];
		snprintf(buf, sizeof(buf), "%s:%d", host, port);
		instance = buf;
	}

	Attrs a;
	a.Str("service.name", service)
	 .Str("service.instance.id", instance.c_str())
	 .Str("deployment.environment.name", g_cvEnvironment->GetString())
	 .Str("server.hostname", g_pHostname ? g_pHostname->GetString() : "")
	 .Int("server.port", port)
	 .Str("game.mod", smutils->GetGameFolderName());
	return a;
}

void OTelExt::EmitSlowTrace(uint64_t endNs, uint64_t gameFrameNs, uint64_t tickCpuNs)
{
	uint64_t gap = 60000000000ULL / (uint64_t)std::max(1, g_cvSlowMaxPerMin->GetInt());
	if (m_LastTraceNs && endNs - m_LastTraceNs < gap) {
		m_SlowSuppressed++;
		return;
	}
	m_LastTraceNs = endNs;

	const std::vector<Span> &spans = m_Profiler.Spans();
	bool haveSpans = m_ProfileOn && m_Profiler.FrameActive();

	// The root covers the earliest callback of the window up to the end of GameFrame.
	uint64_t rootStart = m_FrameStartNs;
	if (haveSpans) {
		for (const Span &s : spans) {
			if (s.startNs && s.startNs < rootStart)
				rootStart = s.startNs;
		}
	}
	uint64_t wallEnd = WallNs();
	auto toWall = [&](uint64_t mono) { return wallEnd - (endNs - mono); };

	TraceDoc doc(BuildResource(), SMEXT_CONF_VERSION);

	Attrs rootAttrs;
	rootAttrs.Int("frame.cpu_us", (int64_t)(tickCpuNs / 1000))
	         .Int("frame.gameframe_us", (int64_t)(gameFrameNs / 1000))
	         .Int("players", playerhelpers->GetNumPlayers())
	         .Int("entities", engine->GetEntityCount())
	         .Str("map", STRING(gpGlobals->mapname))
	         .Int("profiled", haveSpans ? 1 : 0);
	if (haveSpans && m_Profiler.SpansTruncated())
		rootAttrs.Int("spans.truncated", 1);
	uint64_t rootId = doc.Span("ServerFrame", 0, toWall(rootStart), endNs - rootStart, rootAttrs);

	if (haveSpans) {
		uint64_t minSpan = (uint64_t)g_cvMinSpanUs->GetInt() * 1000ULL;
		std::vector<uint64_t> ids(spans.size(), 0);
		size_t emitted = 0;

		for (size_t i = 0; i < spans.size() && emitted < 500; i++) {
			const Span &s = spans[i];
			if (s.durNs < minSpan || s.durNs == 0)
				continue;

			uint64_t parent = rootId;
			if (s.parent >= 0 && ids[s.parent])
				parent = ids[s.parent];

			Attrs a;
			a.Str("sm.plugin", s.stat->plugin.empty() ? "unknown" : s.stat->plugin.c_str())
			 .Str("sm.function", s.stat->func.c_str());
			ids[i] = doc.Span(s.stat->name.c_str(), parent, toWall(s.startNs), s.durNs, a);
			emitted++;
		}
	}

	m_TracesSent++;
	m_Exporter.Post(Endpoint(), "/v1/traces", doc.Finish());
}

void OTelExt::Flush(uint64_t nowNs)
{
	m_NextFlushNs = nowNs + (uint64_t)g_cvInterval->GetInt() * 1000000000ULL;

	// Report collector trouble once per outage, from the game thread.
	std::string lastError = m_Exporter.LastError();
	if (!lastError.empty() && !m_ExporterWasFailing)
		smutils->LogError(myself, "Export failing: %s (endpoint %s)", lastError.c_str(), Endpoint().c_str());
	else if (lastError.empty() && m_ExporterWasFailing)
		smutils->LogMessage(myself, "Export recovered.");
	m_ExporterWasFailing = !lastError.empty();

	uint64_t wallNow = WallNs();
	MetricsDoc doc(BuildResource(), SMEXT_CONF_VERSION, m_StartWallNs, wallNow);
	Attrs none;

	// Busiest callbacks first, capped.
	std::vector<const Stat *> stats;
	stats.reserve(m_Profiler.Stats().size());
	for (const auto &kv : m_Profiler.Stats())
		stats.push_back(kv.second.get());
	std::sort(stats.begin(), stats.end(), [](const Stat *a, const Stat *b) { return a->totalNs > b->totalNs; });
	size_t cap = (size_t)g_cvMaxSeries->GetInt();
	if (stats.size() > cap)
		stats.resize(cap);
	m_FlushedSeries = stats.size();

	auto callbackAttrs = [](const Stat *s) {
		Attrs a;
		a.Str("sm.plugin", s->plugin.empty() ? "unknown" : s->plugin.c_str()).Str("sm.function", s->func.c_str());
		return a;
	};

	doc.BeginHistogram("sourcemod.callback.duration", "ms", "Inclusive duration of SourcePawn callbacks, per plugin and function.");
	for (const Stat *s : stats)
		doc.Hist(callbackAttrs(s), *s);
	doc.End();

	doc.BeginSum("sourcemod.callback.self_time", "ms", "Exclusive duration of SourcePawn callbacks (nested callbacks removed).");
	for (const Stat *s : stats)
		doc.SumDouble(callbackAttrs(s), s->selfNs / 1e6);
	doc.End();

	doc.BeginHistogram("source.frame.duration", "ms", "Wall time of IServerGameDLL::GameFrame (SourceMod timers/forwards that run before it are not included, see source.tick.cpu_time).");
	doc.Hist(none, m_FrameStat);
	doc.End();

	doc.BeginHistogram("source.tick.cpu_time", "ms", "CPU time of the main thread between two server frames (sleep excluded): the real cost of a tick.");
	doc.Hist(none, m_TickCpuStat);
	doc.End();

	doc.BeginHistogram("source.frame.interval", "ms", "Time between two consecutive server frames.");
	doc.Hist(none, m_IntervalStat);
	doc.End();

	doc.BeginSum("source.frame.slow", "{frame}", "Ticks whose CPU time or GameFrame time exceeded sm_otel_slow_ms.");
	doc.SumDouble(none, (double)m_SlowFrames);
	doc.End();

	doc.BeginGauge("source.players", "{player}", "Connected players (bots and SourceTV included).");
	doc.GaugeInt(none, playerhelpers->GetNumPlayers());
	doc.End();

	doc.BeginGauge("source.entities", "{entity}", "Entities in the world.");
	doc.GaugeInt(none, engine->GetEntityCount());
	doc.End();

	doc.BeginGauge("source.tickrate", "Hz", "Configured server tick rate.");
	doc.GaugeDouble(none, gpGlobals->interval_per_tick > 0 ? 1.0 / gpGlobals->interval_per_tick : 0.0);
	doc.End();

	// Per-plugin VM memory, to spot leaks.
	{
		int running = 0;
		std::vector<std::pair<std::string, size_t>> mem;
		SourceMod::IPluginIterator *iter = plsys->GetPluginIterator();
		for (; iter->MorePlugins(); iter->NextPlugin()) {
			SourceMod::IPlugin *pl = iter->GetPlugin();
			if (pl->GetStatus() != SourceMod::Plugin_Running)
				continue;
			running++;
			SourcePawn::IPluginRuntime *rt = pl->GetRuntime();
			if (rt) {
				const char *file = pl->GetFilename();
				mem.emplace_back(PluginLabel(file, strlen(file)), rt->GetMemUsage());
			}
		}
		iter->Release();

		doc.BeginGauge("sourcemod.plugins", "{plugin}", "Running SourceMod plugins.");
		doc.GaugeInt(none, running);
		doc.End();

		doc.BeginGauge("sourcemod.plugin.memory", "By", "SourcePawn VM memory used by a plugin.");
		for (const auto &m : mem) {
			Attrs a;
			a.Str("sm.plugin", m.first.c_str());
			doc.GaugeInt(a, (int64_t)m.second);
		}
		doc.End();
	}

#ifdef PLATFORM_LINUX
	{
		rusage ru;
		if (getrusage(RUSAGE_SELF, &ru) == 0) {
			doc.BeginSum("process.cpu.time", "s", "CPU time used by the srcds process.");
			Attrs user, sys;
			user.Str("process.cpu.state", "user");
			sys.Str("process.cpu.state", "system");
			doc.SumDouble(user, ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6);
			doc.SumDouble(sys, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
			doc.End();
		}

		long pages = 0, resident = 0;
		if (FILE *f = fopen("/proc/self/statm", "r")) {
			if (fscanf(f, "%ld %ld", &pages, &resident) == 2) {
				doc.BeginGauge("process.memory.usage", "By", "Resident memory of the srcds process.");
				doc.GaugeInt(none, (int64_t)resident * sysconf(_SC_PAGESIZE));
				doc.End();
			}
			fclose(f);
		}
	}
#endif

	doc.BeginSum("sm_otel.export.requests", "{request}", "OTLP export requests of this extension, by result.");
	Attrs ok, failed, dropped;
	ok.Str("result", "ok");
	failed.Str("result", "failed");
	dropped.Str("result", "dropped");
	doc.SumDouble(ok, (double)m_Exporter.Sent());
	doc.SumDouble(failed, (double)m_Exporter.Failed());
	doc.SumDouble(dropped, (double)m_Exporter.Dropped());
	doc.End();

	m_Exporter.Post(Endpoint(), "/v1/metrics", doc.Finish());
}

void OTelExt::PrintStatus()
{
	META_CONPRINTF("sm-ext-otel %s\n", SMEXT_CONF_VERSION);
	META_CONPRINTF("  enabled=%d profile=%d, plugin call hooks %s, profiling %s\n", g_cvEnable->GetInt(), g_cvProfile->GetInt(),
		m_HookedFn ? "installed" : "NOT installed (no plugin loaded yet)", m_ProfileOn ? "on" : "off");
	META_CONPRINTF("  hook hits: Execute %" PRIu64 ", Invoke %" PRIu64 ", Invoke(args) %" PRIu64 " (vtable idx %d/%d/%d)\n",
		m_HookHits[0], m_HookHits[1], m_HookHits[2], m_VtblIdx[0], m_VtblIdx[1], m_VtblIdx[2]);
	META_CONPRINTF("  endpoint %s, service '%s', every %ds\n", Endpoint().c_str(), g_cvService->GetString(), g_cvInterval->GetInt());
	META_CONPRINTF("  callbacks tracked: %zu (exporting %zu), calls timed: %" PRIu64 "\n",
		m_Profiler.Stats().size(), m_FlushedSeries, m_Profiler.ScopeCount());
	META_CONPRINTF("  frames: %" PRIu64 ", slow: %" PRIu64 " (> %d ms), traces sent: %" PRIu64 ", rate-limited: %" PRIu64 "\n",
		m_FrameStat.calls, m_SlowFrames, g_cvSlowMs->GetInt(), m_TracesSent, m_SlowSuppressed);
	if (m_FrameStat.calls) {
		META_CONPRINTF("  GameFrame avg %.3f ms, max %.3f ms; tick cpu avg %.3f ms, max %.3f ms\n",
			m_FrameStat.totalNs / 1e6 / m_FrameStat.calls, m_FrameStat.maxNs / 1e6,
			m_TickCpuStat.calls ? m_TickCpuStat.totalNs / 1e6 / m_TickCpuStat.calls : 0.0, m_TickCpuStat.maxNs / 1e6);
	}
	META_CONPRINTF("  export: ok %" PRIu64 ", failed %" PRIu64 ", dropped %" PRIu64 ", %" PRIu64 " bytes\n",
		m_Exporter.Sent(), m_Exporter.Failed(), m_Exporter.Dropped(), m_Exporter.BytesSent());
	std::string err = m_Exporter.LastError();
	if (!err.empty())
		META_CONPRINTF("  last export error: %s\n", err.c_str());
}

void OTelExt::PrintTop(int count, bool bySelf)
{
	std::vector<const Stat *> stats;
	for (const auto &kv : m_Profiler.Stats())
		stats.push_back(kv.second.get());
	std::sort(stats.begin(), stats.end(), [bySelf](const Stat *a, const Stat *b) {
		return bySelf ? a->selfNs > b->selfNs : a->totalNs > b->totalNs;
	});

	META_CONPRINTF("%10s %11s %11s %9s %9s  %s\n", "calls", "total ms", "self ms", "avg us", "max ms", "callback");
	for (int i = 0; i < count && i < (int)stats.size(); i++) {
		const Stat *s = stats[i];
		META_CONPRINTF("%10" PRIu64 " %11.2f %11.2f %9.1f %9.3f  %s\n", s->calls, s->totalNs / 1e6, s->selfNs / 1e6,
			s->calls ? s->totalNs / 1e3 / s->calls : 0.0, s->maxNs / 1e6, s->name.c_str());
	}
	META_CONPRINTF("(sorted by %s time, since the extension loaded)\n", bySelf ? "self" : "total");
}
