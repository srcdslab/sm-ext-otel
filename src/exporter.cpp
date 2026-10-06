#include "exporter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
	#include <winsock2.h>
	#include <ws2tcpip.h>
	typedef SOCKET sock_t;
	#define OTEL_INVALID_SOCK INVALID_SOCKET
	#define OTEL_CLOSE closesocket
	#define OTEL_POLL WSAPoll
	#define OTEL_SEND_FLAGS 0
#else
	#include <sys/socket.h>
	#include <netdb.h>
	#include <unistd.h>
	#include <fcntl.h>
	#include <poll.h>
	#include <errno.h>
	typedef int sock_t;
	#define OTEL_INVALID_SOCK (-1)
	#define OTEL_CLOSE close
	#define OTEL_POLL poll
	#ifdef MSG_NOSIGNAL
		#define OTEL_SEND_FLAGS MSG_NOSIGNAL
	#else
		#define OTEL_SEND_FLAGS 0
	#endif
#endif

namespace otel {

namespace {

constexpr size_t kMaxQueue = 8;
constexpr int kConnectTimeoutMs = 2000;
constexpr int kIoTimeoutMs = 3000;

struct Url
{
	std::string host;
	std::string port = "80";
	std::string base; // path prefix without trailing slash
};

bool ParseUrl(const std::string &url, Url &out, std::string &err)
{
	if (url.compare(0, 8, "https://") == 0) {
		err = "https:// is not supported, use the local collector over http://";
		return false;
	}
	if (url.compare(0, 7, "http://") != 0) {
		err = "endpoint must start with http://";
		return false;
	}

	std::string rest = url.substr(7);
	size_t slash = rest.find('/');
	std::string hostport = rest.substr(0, slash);
	if (slash != std::string::npos)
		out.base = rest.substr(slash);
	while (!out.base.empty() && out.base.back() == '/')
		out.base.pop_back();

	if (!hostport.empty() && hostport[0] == '[') { // [ipv6]:port
		size_t close = hostport.find(']');
		if (close == std::string::npos) {
			err = "bad IPv6 literal in endpoint";
			return false;
		}
		out.host = hostport.substr(1, close - 1);
		if (close + 1 < hostport.size() && hostport[close + 1] == ':')
			out.port = hostport.substr(close + 2);
	} else {
		size_t colon = hostport.rfind(':');
		out.host = hostport.substr(0, colon);
		if (colon != std::string::npos)
			out.port = hostport.substr(colon + 1);
	}

	if (out.host.empty() || out.port.empty()) {
		err = "missing host or port in endpoint";
		return false;
	}
	return true;
}

void SetBlocking(sock_t s, bool blocking)
{
#ifdef _WIN32
	u_long mode = blocking ? 0 : 1;
	ioctlsocket(s, FIONBIO, &mode);
#else
	int flags = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

bool ConnectTimeout(sock_t s, const sockaddr *addr, size_t addrlen, int timeoutMs)
{
	SetBlocking(s, false);
	int rc = connect(s, addr, (int)addrlen);
	if (rc != 0) {
#ifdef _WIN32
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			return false;
#else
		if (errno != EINPROGRESS)
			return false;
#endif
		pollfd pfd;
		pfd.fd = s;
		pfd.events = POLLOUT;
		pfd.revents = 0;
		if (OTEL_POLL(&pfd, 1, timeoutMs) <= 0)
			return false;

		int soErr = 0;
		socklen_t len = sizeof(soErr);
		if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soErr, &len) != 0 || soErr != 0)
			return false;
	}
	SetBlocking(s, true);
	return true;
}

void SetIoTimeout(sock_t s, int ms)
{
#ifdef _WIN32
	DWORD t = (DWORD)ms;
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
#else
	timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

bool SendAll(sock_t s, const char *data, size_t len)
{
	while (len > 0) {
		int n = (int)send(s, data, (int)len, OTEL_SEND_FLAGS);
		if (n <= 0)
			return false;
		data += n;
		len -= (size_t)n;
	}
	return true;
}

bool HttpPost(const Url &u, const std::string &path, const std::string &body, std::string &err)
{
	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo *res = nullptr;
	if (getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res) {
		err = "cannot resolve " + u.host;
		return false;
	}

	sock_t s = OTEL_INVALID_SOCK;
	for (addrinfo *ai = res; ai; ai = ai->ai_next) {
		sock_t c = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (c == OTEL_INVALID_SOCK)
			continue;
		if (ConnectTimeout(c, ai->ai_addr, ai->ai_addrlen, kConnectTimeoutMs)) {
			s = c;
			break;
		}
		OTEL_CLOSE(c);
	}
	freeaddrinfo(res);

	if (s == OTEL_INVALID_SOCK) {
		err = "cannot connect to " + u.host + ":" + u.port;
		return false;
	}

	SetIoTimeout(s, kIoTimeoutMs);

	char head[512];
	int hlen = snprintf(head, sizeof(head),
		"POST %s%s HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: application/json\r\n"
		"Content-Length: %zu\r\nConnection: close\r\n\r\n",
		u.base.c_str(), path.c_str(), u.host.c_str(), u.port.c_str(), body.size());

	bool ok = hlen > 0 && (size_t)hlen < sizeof(head)
		&& SendAll(s, head, (size_t)hlen) && SendAll(s, body.data(), body.size());
	if (!ok) {
		err = "send failed";
		OTEL_CLOSE(s);
		return false;
	}

	char resp[256];
	int n = (int)recv(s, resp, sizeof(resp) - 1, 0);
	OTEL_CLOSE(s);
	if (n <= 0) {
		err = "no response from collector";
		return false;
	}
	resp[n] = '\0';

	int status = 0;
	if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1) {
		err = "malformed HTTP response";
		return false;
	}
	if (status < 200 || status >= 300) {
		char buf[64];
		snprintf(buf, sizeof(buf), "collector answered HTTP %d", status);
		err = buf;
		return false;
	}
	return true;
}

} // namespace

bool Exporter::Start()
{
	if (m_Running)
		return true;

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;
#endif

	m_Stop = false;
	m_Thread = std::thread(&Exporter::Run, this);
	m_Running = true;
	return true;
}

void Exporter::Stop()
{
	if (!m_Running)
		return;

	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Stop = true;
	}
	m_Cv.notify_all();
	m_Thread.join();
	m_Running = false;

#ifdef _WIN32
	WSACleanup();
#endif
}

void Exporter::Post(const std::string &endpoint, const char *path, std::string &&body)
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Queue.size() >= kMaxQueue) {
			m_Queue.pop_front();
			m_Dropped++;
		}
		m_Queue.push_back({endpoint, path, std::move(body)});
	}
	m_Cv.notify_one();
}

std::string Exporter::LastError()
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	return m_LastError;
}

void Exporter::Run()
{
	std::unique_lock<std::mutex> lock(m_Mutex);
	for (;;) {
		m_Cv.wait(lock, [this] { return m_Stop || !m_Queue.empty(); });
		if (m_Stop)
			break;

		Job job = std::move(m_Queue.front());
		m_Queue.pop_front();
		lock.unlock();

		std::string err;
		Url url;
		bool ok = ParseUrl(job.endpoint, url, err) && HttpPost(url, job.path, job.body, err);

		lock.lock();
		if (ok) {
			m_Sent++;
			m_Bytes += job.body.size();
			m_LastError.clear();
		} else {
			m_Failed++;
			m_LastError = err;
		}
	}
}

} // namespace otel
