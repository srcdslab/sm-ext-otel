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
 * Times every public SourcePawn function the host runs (forwards, timers, hooks,
 * commands, SQL callbacks...). The extension hooks IPluginFunction::Execute/Invoke
 * and brackets each call with Enter()/Leave(); a callback is named
 * "plugin.smx::Function" (IPluginFunction::DebugName). Everything runs on the main
 * thread, so the aggregation needs no lock; calls from other threads are ignored.
 */
class Profiler
{
public:
	static constexpr size_t kMaxStats = 4096;
	static constexpr size_t kMaxSpans = 8192;

	// The extension is loaded on the game thread: only that thread is profiled.
	void BindThread() { m_Main = std::this_thread::get_id(); }

	void Enter(SourcePawn::IPluginFunction *fn);
	void Leave();

	// Function pointers are only valid while their plugin is loaded.
	void ClearFnCache() { m_FnCache.clear(); }

	// Drops scopes left open (hooks removed mid-call).
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
		SourcePawn::IPluginFunction *fn;
		bool dup; // Execute() -> Invoke() -> Invoke(args) chain: only the outermost call counts
		Stat *stat;
		uint64_t start;
		uint64_t child;
		int32_t span;
		int32_t parentSpan;
	};

	Stat *Lookup(const char *name);
	Stat *Resolve(SourcePawn::IPluginFunction *fn);

	std::thread::id m_Main;
	std::vector<Frame> m_Stack;
	std::unordered_map<std::string, std::unique_ptr<Stat>> m_Stats;
	std::unordered_map<SourcePawn::IPluginFunction *, Stat *> m_FnCache;
	std::vector<Span> m_Spans;
	bool m_FrameActive = false;
	bool m_SpansTruncated = false;
	uint64_t m_FrameStart = 0;
	uint64_t m_Scopes = 0;
};

} // namespace otel

#endif // _INCLUDE_OTEL_PROFILER_H_
