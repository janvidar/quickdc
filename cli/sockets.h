/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_SOCKETS_H
#define HAVE_QUICKDC_CLI_SOCKETS_H

/*
 * What qdc needs of sockets, on POSIX and on Winsock - its own, since it may not
 * include samurai's. Descriptors stay ints: a Winsock SOCKET is a handle number
 * that fits in one, and INVALID_SOCKET becomes -1.
 */

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <cstddef>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <system_error>
#ifdef _MSC_VER
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace QuickDC::Cli {

#ifdef _WIN32
using NativeSocket = SOCKET;
#else
using NativeSocket = int;
#endif

inline NativeSocket native(int fd)
{
	return static_cast<NativeSocket>(fd);
}

/** Winsock wants starting once before anything else; elsewhere there is nothing to do. */
inline void socketsReady()
{
#ifdef _WIN32
	static const bool started = [] {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	(void) started;
#endif
}

/* Sockets closed on exec, so an agent's children do not hold them; macOS has no SOCK_CLOEXEC or accept4. */

#if !defined(SOCK_CLOEXEC) && !defined(_WIN32)
inline int closingOnExec(int fd)
{
	if (fd != -1) fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}
#endif

#ifdef _WIN32
inline int asDescriptor(SOCKET sd)
{
	return sd == INVALID_SOCKET ? -1 : static_cast<int>(sd);
}
#endif

inline int streamSocket(int family)
{
	socketsReady();
#if defined(_WIN32)
	/* Winsock handles are not inherited unless asked for. */
	return asDescriptor(::socket(family, SOCK_STREAM, 0));
#elif defined(SOCK_CLOEXEC)
	return ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
	return closingOnExec(::socket(family, SOCK_STREAM, 0));
#endif
}

inline int datagramSocket(int family)
{
	socketsReady();
#if defined(_WIN32)
	return asDescriptor(::socket(family, SOCK_DGRAM, 0));
#elif defined(SOCK_CLOEXEC)
	return ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#else
	return closingOnExec(::socket(family, SOCK_DGRAM, 0));
#endif
}

inline int acceptClosing(int listener)
{
#if defined(_WIN32)
	return asDescriptor(::accept(native(listener), nullptr, nullptr));
#elif defined(SOCK_CLOEXEC)
	return ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
#else
	return closingOnExec(::accept(listener, nullptr, nullptr));
#endif
}

inline void closeSocket(int fd)
{
	if (fd == -1) return;
#ifdef _WIN32
	::closesocket(native(fd));
#else
	::close(fd);
#endif
}

inline bool setNonBlocking(int fd)
{
#ifdef _WIN32
	u_long on = 1;
	return ::ioctlsocket(native(fd), FIONBIO, &on) == 0;
#else
	const int flags = ::fcntl(fd, F_GETFL, 0);
	return flags != -1 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

inline int setOption(int fd, int level, int option, int value)
{
	return ::setsockopt(native(fd), level, option, reinterpret_cast<const char*>(&value), sizeof(value));
}

/*
 * A listener's port taken again straight after the last one let it go. Not on
 * Winsock, where SO_REUSEADDR lets a socket bind over one that is listening.
 */
inline void reuseAddress([[maybe_unused]] int fd)
{
#ifndef _WIN32
	setOption(fd, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
}

inline ssize_t sendSome(int fd, const void* data, size_t size)
{
#if defined(_WIN32)
	return ::send(native(fd), static_cast<const char*>(data), static_cast<int>(size), 0);
#elif defined(MSG_NOSIGNAL)
	return ::send(fd, data, size, MSG_NOSIGNAL);
#else
	return ::send(fd, data, size, 0);
#endif
}

inline ssize_t receiveSome(int fd, void* data, size_t size)
{
#ifdef _WIN32
	return ::recv(native(fd), static_cast<char*>(data), static_cast<int>(size), 0);
#else
	return ::recv(fd, data, size, 0);
#endif
}

/** The last socket call's error: errno, or WSAGetLastError() on Winsock. */
inline int socketError()
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

#ifdef _WIN32
inline bool interrupted(int error) { return error == WSAEINTR; }
inline bool wouldBlock(int error) { return error == WSAEWOULDBLOCK; }
inline bool aborted(int error) { return error == WSAECONNABORTED; }
#else
inline bool interrupted(int error) { return error == EINTR; }
inline bool wouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
inline bool aborted(int error) { return error == ECONNABORTED; }
#endif

inline std::string errorText(int error)
{
#ifdef _WIN32
	return std::system_category().message(error);
#else
	return std::strerror(error);
#endif
}

inline std::string lastSocketError()
{
	return errorText(socketError());
}

/** A descriptor to wait on; -1 is skipped, as poll() skips it. */
struct Watched {
	int fd;
	short events;
	short revents = 0;
};

/**
 * poll() over sockets only. Nothing to wait on is a sleep, which WSAPoll() would
 * call an error.
 */
inline int waitFor(Watched* watched, size_t count, int timeout_ms)
{
	std::vector<pollfd> polled;
	std::vector<size_t> at;
	for (size_t n = 0; n < count; ++n)
	{
		watched[n].revents = 0;
		if (watched[n].fd == -1) continue;
		pollfd one {};
		one.fd = native(watched[n].fd);
		one.events = watched[n].events;
		polled.push_back(one);
		at.push_back(n);
	}

	if (polled.empty())
	{
		if (timeout_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
		return 0;
	}

#ifdef _WIN32
	const int ready = ::WSAPoll(polled.data(), static_cast<ULONG>(polled.size()), timeout_ms);
#else
	const int ready = ::poll(polled.data(), static_cast<nfds_t>(polled.size()), timeout_ms);
#endif
	for (size_t n = 0; n < polled.size(); ++n) watched[at[n]].revents = polled[n].revents;
	return ready;
}

inline int waitFor(int fd, short events, int timeout_ms)
{
	Watched one {fd, events};
	return waitFor(&one, 1, timeout_ms);
}

/** A connected pair of stream sockets: socketpair() where there is one, loopback on Windows. */
inline bool streamPair(int fds[2])
{
	fds[0] = fds[1] = -1;
#ifdef _WIN32
	const int listener = streamSocket(AF_INET);
	if (listener == -1) return false;

	sockaddr_in at {};
	at.sin_family = AF_INET;
	at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int size = sizeof(at);
	if (::bind(native(listener), reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0
	    && ::listen(native(listener), 1) == 0
	    && ::getsockname(native(listener), reinterpret_cast<sockaddr*>(&at), &size) == 0)
	{
		fds[1] = streamSocket(AF_INET);
		if (fds[1] != -1 && ::connect(native(fds[1]), reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0)
			fds[0] = acceptClosing(listener);
	}
	closeSocket(listener);

	if (fds[0] == -1)
	{
		closeSocket(fds[1]);
		fds[1] = -1;
		return false;
	}
	return true;
#elif defined(SOCK_CLOEXEC)
	return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0;
#else
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
	closingOnExec(fds[0]);
	closingOnExec(fds[1]);
	return true;
#endif
}

}

#endif // HAVE_QUICKDC_CLI_SOCKETS_H
