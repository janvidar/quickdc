/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "freeport.h"

#include <cerrno>

#include <netinet/in.h>
#include <sys/socket.h>
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

}

int QuickDC::Cli::freePort()
{
	/* The core listens on both, so a port the kernel offers for TCP may still be taken for UDP. */
	for (int attempt = 0; attempt < 32; ++attempt)
	{
		const int tcp = bound(SOCK_STREAM, 0);
		if (tcp == -1) return -1;

		sockaddr_in address {};
		socklen_t length = sizeof(address);
		if (getsockname(tcp, reinterpret_cast<sockaddr*>(&address), &length) != 0)
		{
			const int saved = errno;
			close(tcp);
			errno = saved;
			return -1;
		}

		const int udp = bound(SOCK_DGRAM, ntohs(address.sin_port));
		close(tcp);
		if (udp == -1) continue;
		close(udp);

		return ntohs(address.sin_port);
	}

	errno = EADDRINUSE;
	return -1;
}
