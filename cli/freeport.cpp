/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "freeport.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

/* A socket of 'type' bound to 'port' on every address, or -1. Port 0 lets the kernel pick. */
int bound(int type, in_port_t port)
{
	const int fd = socket(AF_INET, type, 0);
	if (fd == -1) return -1;

	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);

	if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
	{
		const int saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}

	return fd;
}

/*
 * Held until the process exits, by a name in Linux's abstract socket namespace,
 * which needs no file and goes with the process however it ends. Elsewhere every
 * claim succeeds, and two runs at once can still be handed the same port.
 */
/*
 * Held for the life of the process, so no other qdc is offered the port while this
 * one runs: an abstract socket on Linux, a locked file elsewhere.
 */
bool claim(in_port_t port)
{
#ifdef __linux__
	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) return false;

	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	const int length = std::snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1,
	                                 "quickdc-qdc-port-%u", static_cast<unsigned>(port));
	const socklen_t size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + length);

	if (bind(fd, reinterpret_cast<const sockaddr*>(&address), size) != 0)
	{
		close(fd);
		return false;
	}
#else
	const char* tmp = std::getenv("TMPDIR");
	std::string path = (tmp && *tmp) ? tmp : "/tmp";
	path += "/quickdc-qdc-port-" + std::to_string(port) + ".lock";

	const int fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd == -1) return false;
	if (flock(fd, LOCK_EX | LOCK_NB) != 0)
	{
		close(fd);
		return false;
	}
#endif
	return true;
}

/*
 * Below every system's ephemeral range (Linux 32768 and up, macOS and the BSDs 49152),
 * so a port let go here is not handed to some outgoing connection before the core
 * binds it - which, with the core moving up from a taken port, could land it on
 * another core's.
 */
constexpr int FIRST = 20000;
constexpr int LAST = 31999;

}

int QuickDC::Cli::freePort()
{
	std::mt19937 pick(std::random_device{}() ^ static_cast<unsigned>(getpid()));
	std::uniform_int_distribution<int> range(FIRST, LAST);

	/* The core listens on both, so a port free for TCP may still be taken for UDP. */
	for (int attempt = 0; attempt < 256; ++attempt)
	{
		const in_port_t port = static_cast<in_port_t>(range(pick));

		const int tcp = bound(SOCK_STREAM, port);
		if (tcp == -1) continue;
		const int udp = bound(SOCK_DGRAM, port);
		close(tcp);
		if (udp == -1) continue;
		close(udp);

		if (!claim(port)) continue;
		return port;
	}

	errno = EADDRINUSE;
	return -1;
}
