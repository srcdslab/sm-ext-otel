#ifndef _INCLUDE_OTEL_OTLP_H_
#define _INCLUDE_OTEL_OTLP_H_

// Minimal OTLP/HTTP JSON encoder (metrics + traces). uint64 values are strings per
// the OTLP/JSON mapping, trace/span ids are lower-case hex.

#include "profiler.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <random>
#include <string>

namespace otel {

inline void Appendf(std::string &out, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n < sizeof(buf)) {
		out.append(buf, (size_t)n);
		return;
	}
	std::string big((size_t)n + 1, '\0');
	va_start(ap, fmt);
	vsnprintf(&big[0], big.size(), fmt, ap);
	va_end(ap);
	out.append(big.data(), (size_t)n);
}

inline void AppendJsonString(std::string &out, const char *s)
{
	out += '"';
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20)
				Appendf(out, "\\u%04x", c);
			else
				out += (char)c; // UTF-8 passes through untouched
		}
	}
	out += '"';
}

// "attributes" array builder.
class Attrs
{
public:
	Attrs &Str(const char *key, const char *val)
	{
		Key(key);
		m_Json += "{\"stringValue\":";
		AppendJsonString(m_Json, val);
		m_Json += "}}";
		return *this;
	}

	Attrs &Int(const char *key, int64_t val)
	{
		Key(key);
		Appendf(m_Json, "{\"intValue\":\"%" PRId64 "\"}}", val);
		return *this;
	}

	std::string Json() const { return "[" + m_Json + "]"; }

private:
	void Key(const char *key)
	{
		if (!m_Json.empty())
			m_Json += ',';
		m_Json += "{\"key\":";
		AppendJsonString(m_Json, key);
		m_Json += ",\"value\":";
	}

	std::string m_Json;
};

inline void AppendResource(std::string &out, const Attrs &resource)
{
	Appendf(out, "\"resource\":{\"attributes\":%s}", resource.Json().c_str());
}

inline void AppendScope(std::string &out, const char *version)
{
	out += "\"scope\":{\"name\":\"sm-ext-otel\",\"version\":";
	AppendJsonString(out, version);
	out += '}';
}

/**
 * Builds one resourceMetrics document. Usage: BeginX(), N x point, End().
 * All series are cumulative since startNs.
 */
class MetricsDoc
{
public:
	MetricsDoc(const Attrs &resource, const char *version, uint64_t startNs, uint64_t nowNs)
		: m_Start(startNs), m_Now(nowNs)
	{
		m_Json = "{\"resourceMetrics\":[{";
		AppendResource(m_Json, resource);
		m_Json += ",\"scopeMetrics\":[{";
		AppendScope(m_Json, version);
		m_Json += ",\"metrics\":[";
	}

	void BeginHistogram(const char *name, const char *unit, const char *desc)
	{
		Begin(name, unit, desc);
		m_Json += "\"histogram\":{\"aggregationTemporality\":2,\"dataPoints\":[";
	}

	void BeginSum(const char *name, const char *unit, const char *desc)
	{
		Begin(name, unit, desc);
		m_Json += "\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,\"dataPoints\":[";
	}

	void BeginGauge(const char *name, const char *unit, const char *desc)
	{
		Begin(name, unit, desc);
		m_Json += "\"gauge\":{\"dataPoints\":[";
	}

	void End()
	{
		m_Json += "]}}";
		m_Open = false;
	}

	// Histogram point, durations exported in milliseconds.
	void Hist(const Attrs &attrs, const Stat &st)
	{
		Point(attrs, /*cumulative=*/true);
		Appendf(m_Json, ",\"count\":\"%" PRIu64 "\",\"sum\":%.6f,\"bucketCounts\":[", st.calls, st.totalNs / 1e6);
		for (int i = 0; i <= kNumBounds; i++)
			Appendf(m_Json, "%s\"%" PRIu64 "\"", i ? "," : "", st.buckets[i]);
		m_Json += "],\"explicitBounds\":[";
		for (int i = 0; i < kNumBounds; i++)
			Appendf(m_Json, "%s%.3f", i ? "," : "", kBoundsNs[i] / 1e6);
		Appendf(m_Json, "],\"min\":%.6f,\"max\":%.6f}", st.calls ? st.minNs / 1e6 : 0.0, st.maxNs / 1e6);
	}

	void SumDouble(const Attrs &attrs, double v)
	{
		Point(attrs, true);
		Appendf(m_Json, ",\"asDouble\":%.6f}", v);
	}

	void GaugeInt(const Attrs &attrs, int64_t v)
	{
		Point(attrs, false);
		Appendf(m_Json, ",\"asInt\":\"%" PRId64 "\"}", v);
	}

	void GaugeDouble(const Attrs &attrs, double v)
	{
		Point(attrs, false);
		Appendf(m_Json, ",\"asDouble\":%.6f}", v);
	}

	std::string Finish()
	{
		if (m_Open)
			End();
		m_Json += "]}]}]}";
		return std::move(m_Json);
	}

private:
	void Begin(const char *name, const char *unit, const char *desc)
	{
		if (m_Open)
			End();
		if (m_Metrics++)
			m_Json += ',';
		m_Json += "{\"name\":";
		AppendJsonString(m_Json, name);
		m_Json += ",\"unit\":";
		AppendJsonString(m_Json, unit);
		m_Json += ",\"description\":";
		AppendJsonString(m_Json, desc);
		m_Json += ',';
		m_Open = true;
		m_Points = 0;
	}

	// Opens a data point up to (and excluding) the value fields; the caller closes it.
	void Point(const Attrs &attrs, bool cumulative)
	{
		if (m_Points++)
			m_Json += ',';
		Appendf(m_Json, "{\"attributes\":%s", attrs.Json().c_str());
		if (cumulative)
			Appendf(m_Json, ",\"startTimeUnixNano\":\"%" PRIu64 "\"", m_Start);
		Appendf(m_Json, ",\"timeUnixNano\":\"%" PRIu64 "\"", m_Now);
	}

	std::string m_Json;
	uint64_t m_Start;
	uint64_t m_Now;
	int m_Metrics = 0;
	int m_Points = 0;
	bool m_Open = false;
};

inline uint64_t RandomU64()
{
	static thread_local std::mt19937_64 rng{std::random_device{}() ^ (uint64_t)WallNs()};
	uint64_t v;
	do { v = rng(); } while (v == 0);
	return v;
}

/** Builds one resourceSpans document for a single trace. */
class TraceDoc
{
public:
	TraceDoc(const Attrs &resource, const char *version)
	{
		m_TraceHi = RandomU64();
		m_TraceLo = RandomU64();
		m_Json = "{\"resourceSpans\":[{";
		AppendResource(m_Json, resource);
		m_Json += ",\"scopeSpans\":[{";
		AppendScope(m_Json, version);
		m_Json += ",\"spans\":[";
	}

	// parentId 0 = root span. Returns the new span id.
	uint64_t Span(const char *name, uint64_t parentId, uint64_t startWallNs, uint64_t durNs, const Attrs &attrs)
	{
		uint64_t id = RandomU64();
		if (m_Count++)
			m_Json += ',';
		Appendf(m_Json, "{\"traceId\":\"%016" PRIx64 "%016" PRIx64 "\",\"spanId\":\"%016" PRIx64 "\"",
			m_TraceHi, m_TraceLo, id);
		if (parentId)
			Appendf(m_Json, ",\"parentSpanId\":\"%016" PRIx64 "\"", parentId);
		m_Json += ",\"name\":";
		AppendJsonString(m_Json, name);
		Appendf(m_Json, ",\"kind\":1,\"startTimeUnixNano\":\"%" PRIu64 "\",\"endTimeUnixNano\":\"%" PRIu64 "\",\"attributes\":%s}",
			startWallNs, startWallNs + durNs, attrs.Json().c_str());
		return id;
	}

	std::string Finish()
	{
		m_Json += "]}]}]}";
		return std::move(m_Json);
	}

private:
	std::string m_Json;
	uint64_t m_TraceHi, m_TraceLo;
	int m_Count = 0;
};

} // namespace otel

#endif // _INCLUDE_OTEL_OTLP_H_
