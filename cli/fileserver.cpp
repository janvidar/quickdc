/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "fileserver.h"
#include "sockets.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>


namespace {

const int READ_TIMEOUT_MS = 5000;
const size_t REQUEST_LIMIT = 8192;

void writeAll(int fd, const std::string& data)
{
	size_t sent = 0;
	while (sent < data.size())
	{
		const ssize_t n = QuickDC::Cli::sendSome(fd, data.data() + sent, data.size() - sent);
		if (n < 0 && QuickDC::Cli::interrupted(QuickDC::Cli::socketError())) continue;
		if (n <= 0) return;
		sent += static_cast<size_t>(n);
	}
}

std::string response(const char* status, const std::string& body)
{
	return std::string("HTTP/1.1 ") + status + "\r\nContent-Length: " + std::to_string(body.size())
		+ "\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n" + body;
}

}

QuickDC::Cli::FileServer::~FileServer()
{
	if (thread.joinable())
	{
		const char stop = 0;
		[[maybe_unused]] const ssize_t ignored = QuickDC::Cli::sendSome(wake[1], &stop, 1);
		thread.join();
	}
	for (int fd : {listener, wake[0], wake[1]})
		QuickDC::Cli::closeSocket(fd);
}

bool QuickDC::Cli::FileServer::start(int port, std::string directory, std::string& why)
{
	root = std::move(directory);

	/* A socket pair rather than a pipe, so the wake-up can be polled with the listener on Winsock too. */
	listener = QuickDC::Cli::streamSocket(AF_INET);
	if (listener == -1 || !QuickDC::Cli::streamPair(wake))
	{
		why = QuickDC::Cli::lastSocketError();
		return false;
	}

	QuickDC::Cli::reuseAddress(listener);

	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(static_cast<uint16_t>(port));

	if (::bind(QuickDC::Cli::native(listener), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
		|| ::listen(QuickDC::Cli::native(listener), 16) != 0)
	{
		why = "cannot listen on port " + std::to_string(port) + ": " + QuickDC::Cli::lastSocketError();
		return false;
	}

	thread = std::thread([this] { run(); });
	return true;
}

void QuickDC::Cli::FileServer::run()
{
	while (true)
	{
		QuickDC::Cli::Watched watched[2] = {{listener, POLLIN}, {wake[0], POLLIN}};
		if (QuickDC::Cli::waitFor(watched, 2, -1) < 0)
		{
			if (QuickDC::Cli::interrupted(QuickDC::Cli::socketError())) continue;
			return;
		}
		if (watched[1].revents) return;
		if (!(watched[0].revents & POLLIN)) continue;

		const int client = QuickDC::Cli::acceptClosing(listener);
		if (client == -1) continue;
		answer(client);
		QuickDC::Cli::closeSocket(client);
	}
}

void QuickDC::Cli::FileServer::answer(int client)
{
	std::string request;
	while (request.find("\r\n\r\n") == std::string::npos && request.size() < REQUEST_LIMIT)
	{
		if (QuickDC::Cli::waitFor(client, POLLIN, READ_TIMEOUT_MS) <= 0) return;

		char buffer[1024];
		const ssize_t n = QuickDC::Cli::receiveSome(client, buffer, sizeof(buffer));
		if (n < 0 && QuickDC::Cli::interrupted(QuickDC::Cli::socketError())) continue;
		if (n <= 0) return;
		request.append(buffer, static_cast<size_t>(n));
	}

	if (request.compare(0, 4, "GET ") != 0)
	{
		writeAll(client, response("405 Method Not Allowed", std::string()));
		return;
	}

	std::string path = request.substr(4, request.find(' ', 4) - 4);
	path = path.substr(0, path.find_first_of("?#"));

	if (path.empty() || path[0] != '/' || path.find("..") != std::string::npos)
	{
		writeAll(client, response("400 Bad Request", std::string()));
		return;
	}

	std::ifstream file(root + path, std::ios::binary);
	if (!file)
	{
		writeAll(client, response("404 Not Found", std::string()));
		return;
	}

	const std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	/* Counted first, so a script that has heard what the core made of it sees the count. */
	++files;
	writeAll(client, response("200 OK", body));
}
