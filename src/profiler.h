#ifndef _INCLUDE_OTEL_PROFILER_H_
#define _INCLUDE_OTEL_PROFILER_H_

#include <sp_vm_api.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace otel {

inline uint64_t NowNs()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline uint64_t WallNs()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

// Histogram bucket upper bounds, in nanoseconds (exported as milliseconds).
constexpr int kNumBounds = 13;
extern const uint64_t kBoundsNs[kNumBounds];

// Aggregated timing of one callback (or of the frame itself). Cumulative since
// the extension was loaded.
struct Stat
{
	std::string name;     // full "plugin.smx::Function"
	std::string plugin;   // "plugin"
	std::string func;     // "Function"
	uint64_t calls = 0;
	uint64_t totalNs = 0; // inclusive time
	uint64_t selfNs = 0;  // exclusive time (total minus nested scopes)
	uint64_t minNs = UINT64_MAX;
	uint64_t maxNs = 0;
	uint64_t buckets[kNumBounds + 1] = {};

	void Record(uint64_t durNs, uint64_t selfTimeNs)
	{
		calls++;
		totalNs += durNs;
		selfNs += selfTimeNs;
		if (durNs < minNs) minNs = durNs;
		if (durNs > maxNs) maxNs = durNs;
		int i = 0;
		while (i < kNumBounds && durNs > kBoundsNs[i]) i++;
		buckets[i]++;
	}
};

// One callback invocation inside the frame being recorded (for slow-frame traces).
struct Span
{
	Stat *stat;
	uint64_t startNs;
	uint64_t durNs;
	int32_t parent; // index in the span list, -1 = frame root
};

// Strips directories and the ".smx" extension: "custom/Foo.smx" -> "Foo".
std::string PluginLabel(const char *path, size_t len);

/**
 * SourcePawn profiling tool. SourcePawn calls EnterScope/LeaveScope around every
 * public function it executes (forwards, timers, hooks, commands, SQL callbacks...);
 * the scope name is "plugin.smx::Function". Everything runs on the main thread, so
 * the aggregation needs no lock; scopes entered from other threads are ignored.
 */
class Profiler : public SourcePawn::IProfilingTool
{
public:
	static constexpr size_t kMaxStats = 4096;
	static constexpr size_t kMaxSpans = 8192;

	// The extension is loaded on the game thread: only that thread is profiled.
	void BindThread() { m_Main = std::this_thread::get_id(); }

	// IProfilingTool
	const char *Name() override { return "otel"; }
	const char *Description() override { return "OpenTelemetry exporter (sm-ext-otel)"; }
	void RenderHelp(void (*render)(const char *fmt, ...)) override
	{
		render("sm-ext-otel profiles continuously; see sm_otel_status / sm_otel_top.");
	}
	bool Start() override { return false; }
	void Stop(void (*render)(const char *fmt, ...)) override {}
	void Dump() override {}
	bool IsActive() override { return true; }
	bool IsAttached() override { return true; }
	void EnterScope(const char *group, const char *name) override;
	void LeaveScope() override;

	// Called when the tool is (re)attached or detached from the VM.
	void ResetStack() { m_Stack.clear(); }

	void BeginFrame();
	void EndFrame() { m_FrameActive = false; }
	const std::vector<Span> &Spans() const { return m_Spans; }
	bool SpansTruncated() const { return m_SpansTruncated; }
	uint64_t FrameStartNs() const { return m_FrameStart; }
	uint64_t ScopeCount() const { return m_Scopes; }

	const std::unordered_map<std::string, std::unique_ptr<Stat>> &Stats() const { return m_Stats; }

private:
	struct Frame
	{
		Stat *stat;
		uint64_t start;
		uint64_t child;
		int32_t span;
		int32_t parentSpan;
	};

	Stat *Lookup(const char *name);

	std::thread::id m_Main;
	std::vector<Frame> m_Stack;
	std::unordered_map<std::string, std::unique_ptr<Stat>> m_Stats;
	std::unordered_map<const char *, Stat *> m_PtrCache; // DebugName() pointers are stable per function
	std::vector<Span> m_Spans;
	bool m_FrameActive = false;
	bool m_SpansTruncated = false;
	uint64_t m_FrameStart = 0;
	uint64_t m_Scopes = 0;
};

} // namespace otel

#endif // _INCLUDE_OTEL_PROFILER_H_
