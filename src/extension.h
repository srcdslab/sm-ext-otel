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
	public IConCommandBaseAccessor
{
public:
	virtual bool SDK_OnLoad(char *error, size_t maxlength, bool late);
	virtual void SDK_OnUnload();
	virtual bool SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlength, bool late);

	// IConCommandBaseAccessor
	virtual bool RegisterConCommandBase(ConCommandBase *pVar);

public: // SourceHook, GameFrame
	void Hook_GameFramePre(bool simulating);
	void Hook_GameFramePost(bool simulating);

public: // console
	void PrintStatus();
	void PrintTop(int count, bool bySelf);

private:
	bool AcquireSourcePawn();
	void SyncProfiler(bool want);
	void Flush(uint64_t nowNs);
	void EmitSlowTrace(uint64_t endNs, uint64_t durNs);
	otel::Attrs BuildResource();
	std::string Endpoint();

private:
	otel::Profiler m_Profiler;
	otel::Exporter m_Exporter;

	std::unique_ptr<ILibrary> m_SpLib;
	SourcePawn::ISourcePawnEngine2 *m_Engine2 = nullptr;
	std::string m_SpVersion;
	bool m_Attached = false;
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
	uint64_t m_ScopesAtFlush = 0;
	size_t m_FlushedSeries = 0;
	bool m_ExporterWasFailing = false;
	uint64_t m_FailedAtFlush = 0;
};

extern OTelExt g_OTel;

#endif // _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
