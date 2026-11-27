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

#include "sockets.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/file.h>
#endif

#include <filesystem>

namespace {

/* A socket of 'type' bound to 'port' on every address, or -1. Port 0 lets the kernel pick. */
int bound(int type, uint16_t port)
{
	const int fd = type == SOCK_STREAM ? QuickDC::Cli::streamSocket(AF_INET) : QuickDC::Cli::datagramSocket(AF_INET);
	if (fd == -1) return -1;

	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);

	if (::bind(QuickDC::Cli::native(fd), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
	{
		QuickDC::Cli::closeSocket(fd);
		return -1;
	}

	return fd;
}

/*
 * Held for the life of the process, so no other qdc is offered the port while this
 * one runs: an abstract socket on Linux, a named mutex on Windows, a locked file
 * elsewhere. Each goes with the process however it ends.
 */
bool claim(uint16_t port)
{
#if defined(_WIN32)
	const std::string name = "Local\\quickdc-qdc-port-" + std::to_string(port);
	HANDLE held = CreateMutexA(nullptr, FALSE, name.c_str());
	if (!held) return false;
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		CloseHandle(held);
		return false;
	}
#elif defined(__linux__)
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
	std::error_code ec;
	std::string path = std::filesystem::temp_directory_path(ec).string();
	if (ec || path.empty()) path = "/tmp";
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
	std::mt19937 pick(std::random_device{}());
	std::uniform_int_distribution<int> range(FIRST, LAST);

	/* The core listens on both, so a port free for TCP may still be taken for UDP. */
	for (int attempt = 0; attempt < 256; ++attempt)
	{
		const uint16_t port = static_cast<uint16_t>(range(pick));

		const int tcp = bound(SOCK_STREAM, port);
		if (tcp == -1) continue;
		const int udp = bound(SOCK_DGRAM, port);
		QuickDC::Cli::closeSocket(tcp);
		if (udp == -1) continue;
		QuickDC::Cli::closeSocket(udp);

		if (!claim(port)) continue;
		return port;
	}

	return -1;
}
