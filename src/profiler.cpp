#include "profiler.h"

#include <cstring>

namespace otel {

// 0.01 ms .. 250 ms
const uint64_t kBoundsNs[kNumBounds] = {
	10000ULL, 50000ULL, 100000ULL, 250000ULL, 500000ULL, 1000000ULL, 2500000ULL,
	5000000ULL, 10000000ULL, 25000000ULL, 50000000ULL, 100000000ULL, 250000000ULL
};

std::string PluginLabel(const char *path, size_t len)
{
	size_t start = 0;
	for (size_t i = 0; i < len; i++) {
		if (path[i] == '/' || path[i] == '\\')
			start = i + 1;
	}
	if (len - start > 4 && strncmp(path + len - 4, ".smx", 4) == 0)
		len -= 4;
	return std::string(path + start, len - start);
}

Stat *Profiler::Lookup(const char *name)
{
	auto it = m_Stats.find(name);
	if (it != m_Stats.end())
		return it->second.get();

	if (m_Stats.size() >= kMaxStats)
		return nullptr;

	auto created = std::make_unique<Stat>();
	created->name = name;
	const char *sep = strstr(name, "::");
	if (sep) {
		created->plugin = PluginLabel(name, sep - name);
		created->func = sep + 2;
	} else {
		created->func = name;
	}
	Stat *stat = created.get();
	m_Stats.emplace(created->name, std::move(created));
	return stat;
}

Stat *Profiler::Resolve(SourcePawn::IPluginFunction *fn)
{
	auto it = m_FnCache.find(fn);
	if (it != m_FnCache.end())
		return it->second;

	const char *name = fn->DebugName();
	Stat *stat = name ? Lookup(name) : nullptr;
	m_FnCache[fn] = stat;
	return stat;
}

void Profiler::BeginFrame()
{
	m_Spans.clear();
	m_SpansTruncated = false;
	m_FrameActive = true;
	m_FrameStart = NowNs();
}

void Profiler::Enter(SourcePawn::IPluginFunction *fn)
{
	if (std::this_thread::get_id() != m_Main)
		return;

	Frame f;
	f.fn = fn;
	f.dup = false;
	f.stat = nullptr;
	f.child = 0;
	f.span = -1;
	f.parentSpan = -1;

	if (!m_Stack.empty() && m_Stack.back().fn == fn) {
		f.dup = true;
		f.start = 0;
		m_Stack.push_back(f);
		return;
	}

	f.stat = Resolve(fn);

	if (!m_Stack.empty()) {
		const Frame &p = m_Stack.back();
		f.parentSpan = p.span >= 0 ? p.span : p.parentSpan;
	}

	if (f.stat && m_FrameActive) {
		if (m_Spans.size() < kMaxSpans) {
			f.span = (int32_t)m_Spans.size();
			m_Spans.push_back({f.stat, 0, 0, f.parentSpan});
		} else {
			m_SpansTruncated = true;
		}
	}

	m_Scopes++;
	f.start = NowNs();
	if (f.span >= 0)
		m_Spans[f.span].startNs = f.start;
	m_Stack.push_back(f);
}

void Profiler::Leave()
{
	if (std::this_thread::get_id() != m_Main)
		return;
	if (m_Stack.empty())
		return;

	uint64_t end = NowNs();
	Frame f = m_Stack.back();
	m_Stack.pop_back();
	if (f.dup)
		return;

	uint64_t dur = end - f.start;
	if (!m_Stack.empty())
		m_Stack.back().child += dur;

	if (f.stat) {
		f.stat->Record(dur, dur > f.child ? dur - f.child : 0);
		if (f.span >= 0 && (size_t)f.span < m_Spans.size())
			m_Spans[f.span].durNs = dur;
	}
}

} // namespace otel
