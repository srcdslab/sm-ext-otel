#ifndef _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_

/**
 * @file extension.h
 * @brief OpenTelemetry APM for SourceMod.
 */

#include "smsdk_ext.h"

// sp::CallArgs, and the Invoke(args) entry point that takes it, exist from extension API 9
// (SM 1.13). Older SM (1.12) only has Execute / Invoke(result).
#define OTEL_HAS_INVOKE_ARGS (SMINTERFACE_EXTENSIONAPI_VERSION >= 9)

#include "exporter.h"
#include "otlp.h"
#include "profiler.h"

#include <memory>

class OTelExt :
	public SDKExtension,
	public IConCommandBaseAccessor,
	public SourceMod::IPluginsListener
{
public:
	virtual bool SDK_OnLoad(char *error, size_t maxlength, bool late);
	virtual void SDK_OnUnload();
	virtual void SDK_OnAllLoaded();
	virtual bool SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlength, bool late);

	// IConCommandBaseAccessor
	virtual bool RegisterConCommandBase(ConCommandBase *pVar);

	// IPluginsListener
	virtual void OnPluginLoaded(SourceMod::IPlugin *plugin);
	virtual void OnPluginUnloaded(SourceMod::IPlugin *plugin);
	virtual void OnPluginDestroyed(SourceMod::IPlugin *plugin);

public: // SourceHook
	void Hook_GameFramePre(bool simulating);
	void Hook_GameFramePost(bool simulating);
	int Hook_ExecutePre(cell_t *result);
	int Hook_ExecutePost(cell_t *result);
	bool Hook_InvokePre(cell_t *result);
	bool Hook_InvokePost(cell_t *result);
#if OTEL_HAS_INVOKE_ARGS
	bool Hook_InvokeArgsPre(const sp::CallArgs &args, cell_t *result);
	bool Hook_InvokeArgsPost(const sp::CallArgs &args, cell_t *result);
#endif

public: // console
	void PrintStatus();
	void PrintTop(int count, bool bySelf);

private:
	bool TryInstallHooks(SourcePawn::IPluginRuntime *runtime);
	void RemoveHooks();
	void Flush(uint64_t nowNs);
	void EmitSlowTrace(uint64_t endNs, uint64_t gameFrameNs, uint64_t tickCpuNs);
	otel::Attrs BuildResource();
	std::string Endpoint();

private:
	otel::Profiler m_Profiler;
	otel::Exporter m_Exporter;

	SourcePawn::IPluginFunction *m_HookedFn = nullptr; // any function: SourceHook patches the shared vtable
	bool m_ProfileOn = false;  // toggled between frames only, so Enter/Leave stay paired
	uint64_t m_HookHits[3] = {0, 0, 0}; // Execute, Invoke, Invoke(args): diagnostics
	int m_VtblIdx[3] = {-1, -1, -1};

	otel::Stat m_FrameStat;
	otel::Stat m_IntervalStat;
	otel::Stat m_TickCpuStat;
	uint64_t m_PrevPostCpuNs = 0;
	uint64_t m_SlowFrames = 0;
	uint64_t m_SlowSuppressed = 0;
	uint64_t m_TracesSent = 0;

	uint64_t m_StartWallNs = 0;
	uint64_t m_FrameStartNs = 0;
	uint64_t m_PrevFrameStartNs = 0;
	uint64_t m_NextFlushNs = 0;
	uint64_t m_LastTraceNs = 0;
	size_t m_FlushedSeries = 0;
	bool m_ExporterWasFailing = false;
};

extern OTelExt g_OTel;

#endif // _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
