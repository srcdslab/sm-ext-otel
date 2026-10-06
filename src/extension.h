#ifndef _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_

/**
 * @file extension.h
 * @brief OpenTelemetry APM for SourceMod.
 */

#include "smsdk_ext.h"
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
	bool Hook_InvokeArgsPre(const sp::CallArgs &args, cell_t *result);
	bool Hook_InvokeArgsPost(const sp::CallArgs &args, cell_t *result);

public: // console
	void PrintStatus();
	void PrintTop(int count, bool bySelf);

private:
	bool TryInstallHooks(SourcePawn::IPluginRuntime *runtime);
	void RemoveHooks();
	void Flush(uint64_t nowNs);
	void EmitSlowTrace(uint64_t endNs, uint64_t durNs);
	otel::Attrs BuildResource();
	std::string Endpoint();

private:
	otel::Profiler m_Profiler;
	otel::Exporter m_Exporter;

	SourcePawn::IPluginFunction *m_HookedFn = nullptr; // any function: SourceHook patches the shared vtable
	bool m_ProfileOn = false;  // toggled between frames only, so Enter/Leave stay paired
	bool m_FrameProfiled = false;

	otel::Stat m_FrameStat;
	otel::Stat m_IntervalStat;
	uint64_t m_SlowFrames = 0;
	uint64_t m_SlowSuppressed = 0;
	uint64_t m_TracesSent = 0;

	uint64_t m_StartWallNs = 0;
	uint64_t m_FrameStartNs = 0;
	uint64_t m_FrameWallNs = 0;
	uint64_t m_PrevFrameStartNs = 0;
	uint64_t m_NextFlushNs = 0;
	uint64_t m_LastTraceNs = 0;
	uint64_t m_LastFlushNs = 0;
	size_t m_FlushedSeries = 0;
	bool m_ExporterWasFailing = false;
	uint64_t m_FailedAtFlush = 0;
};

extern OTelExt g_OTel;

#endif // _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
