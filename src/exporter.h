#ifndef _INCLUDE_OTEL_EXPORTER_H_
#define _INCLUDE_OTEL_EXPORTER_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace otel {

/**
 * Background OTLP/HTTP sender. The game thread builds the JSON payload and hands
 * it over with Post(); a worker thread does the (blocking, time-limited) HTTP POST
 * so a slow or dead collector can never stall the server frame.
 *
 * Plain http:// only: point it at the local OpenTelemetry Collector, which handles
 * TLS, batching and retries towards the backend.
 */
class Exporter
{
public:
	~Exporter() { Stop(); }

	bool Start();
	void Stop();

	// Queue a payload for POST <endpoint><path>. Never blocks; drops the oldest job when full.
	void Post(const std::string &endpoint, const char *path, std::string &&body);

	uint64_t Sent() const { return m_Sent; }
	uint64_t Failed() const { return m_Failed; }
	uint64_t Dropped() const { return m_Dropped; }
	uint64_t BytesSent() const { return m_Bytes; }
	std::string LastError();

private:
	struct Job
	{
		std::string endpoint;
		std::string path;
		std::string body;
	};

	void Run();

	std::thread m_Thread;
	std::mutex m_Mutex;
	std::condition_variable m_Cv;
	std::deque<Job> m_Queue;
	std::string m_LastError;
	bool m_Stop = false;
	bool m_Running = false;
	std::atomic<uint64_t> m_Sent{0};
	std::atomic<uint64_t> m_Failed{0};
	std::atomic<uint64_t> m_Dropped{0};
	std::atomic<uint64_t> m_Bytes{0};
};

} // namespace otel

#endif // _INCLUDE_OTEL_EXPORTER_H_
