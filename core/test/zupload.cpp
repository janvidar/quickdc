/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Drives a real Upload and ZUpload over a socketpair and checks that what comes
 * out the other end is what went in.
 *
 * The codec loop is covered by the autotests; what needs a socket is everything
 * around it - the short-write buffering, the flush after the last input byte,
 * and the fact that a compressed transfer cannot finish just because the file
 * position reached the end.
 *
 * The send buffer is made deliberately small so writes fail part way and have
 * to be retried, which is the case that cannot happen with a loopback socket
 * left at its default size.
 */

#include "quickdc.h"
#include "network/transfer.h"
#include "network/transfermanager.h"

#include <samurai/io/file.h>
#include <samurai/io/compression.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/net/bandwidth.h>
#include <samurai/messagehandler.h>
#include <samurai/timer.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <span>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
			failures++; \
		} \
	} while (0)

#define SMALL_SNDBUF 2048

class Listener : public QuickDC::PrivateTransferListener
{
	public:
		Listener() : stopped(false) { }
		void onTransferStopped() { stopped = true; }
		bool stopped;
};

static std::string testfile_path;

static bool write_test_file(const std::string& data)
{
	Samurai::IO::File file(testfile_path);
	if (!file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate)) return false;

	bool ok = (file.write(data.data(), data.size()) == (ssize_t) data.size());
	file.close();
	return ok;
}

/**
 * Run a transfer to completion, draining the far end as it goes.
 *
 * @param compress use ZUpload rather than Upload.
 * @param out      everything that arrived on the far end.
 * @return false if the transfer never finished.
 */
static bool run_transfer(bool compress, const std::string& data, std::vector<char>& out, double& ratio)
{
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;

	/* Non-blocking, so a full send buffer reports EAGAIN instead of waiting. */
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	fcntl(fds[1], F_SETFL, O_NONBLOCK);

	int sndbuf = SMALL_SNDBUF;
	setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

	Samurai::IO::Net::InetSocketAddress addr("127.0.0.1", 0, Samurai::IO::Net::InetAddress::Version::IPv4);
	auto sock = Samurai::IO::Net::Socket::create(fds[0], addr);

	QuickDC::TransferManager manager;
	Listener listener;
	Samurai::IO::File file(testfile_path);

	QuickDC::Transfer* transfer = 0;
	if (compress)
		transfer = new QuickDC::ZUpload(&manager, &listener, sock, &file, 0, (off_t) data.size());
	else
		transfer = new QuickDC::Upload(&manager, &listener, sock, &file, 0, (off_t) data.size());

	bool finished = false;
	int spins = 0;

	while (!finished && spins++ < 1000000)
	{
		finished = transfer->exec();

		char buf[4096];
		ssize_t got;
		while ((got = ::read(fds[1], buf, sizeof(buf))) > 0)
			out.insert(out.end(), buf, buf + got);
	}

	/* The last write may still be sitting in the socket buffer. */
	char buf[4096];
	ssize_t got;
	while ((got = ::read(fds[1], buf, sizeof(buf))) > 0)
		out.insert(out.end(), buf, buf + got);

	ratio = transfer->getRatio();
	bool complete = transfer->isFinished() && finished;

	/* The manager defers this, so run it as the message loop would. */
	manager.process();

	delete transfer;
	sock.reset();
	::close(fds[1]);

	return complete && listener.stopped;
}

static bool inflate_all(const std::vector<char>& in, std::vector<char>& out)
{
	Samurai::IO::GzipDecompressor codec;

	std::vector<char> chunk(65536);
	std::span<const char> input(in);
	int spins = 0;

	for (;;)
	{
		if (spins++ > 100000) return false;

		const Samurai::IO::Codec::Progress p = codec.step(input, chunk);
		if (p.status == Samurai::IO::Codec::Status::Error) return false;

		if (p.produced) out.insert(out.end(), chunk.begin(), chunk.begin() + p.produced);
		input = input.subspan(p.consumed);

		if (p.status == Samurai::IO::Codec::Status::StreamEnd) break;
		if (!p.consumed && !p.produced) break;
	}
	return true;
}

static void test_plain(const char* label, const std::string& data)
{
	CHECK(write_test_file(data));

	std::vector<char> wire;
	double ratio = 0.0;
	if (!run_transfer(false, data, wire, ratio))
	{
		printf("FAIL %s: plain transfer did not finish\n", label);
		failures++;
		return;
	}

	/* An uncompressed upload has to be byte for byte what is on disk. */
	CHECK(wire.size() == data.size());
	if (wire.size() == data.size() && !data.empty())
		CHECK(memcmp(&wire[0], data.data(), data.size()) == 0);

	printf("  ok %-26s plain      %8zu -> %8zu bytes\n", label, data.size(), wire.size());
}

static void test_compressed(const char* label, const std::string& data, bool expect_smaller)
{
	CHECK(write_test_file(data));

	std::vector<char> wire;
	double ratio = 0.0;
	if (!run_transfer(true, data, wire, ratio))
	{
		printf("FAIL %s: compressed transfer did not finish\n", label);
		failures++;
		return;
	}

	/*
	 * The stream has to be complete. Stopping when the file position reaches
	 * the end truncates it, because deflate still owes us the flush.
	 */
	std::vector<char> plain;
	if (!inflate_all(wire, plain))
	{
		printf("FAIL %s: the compressed stream does not inflate\n", label);
		failures++;
		return;
	}

	CHECK(plain.size() == data.size());
	if (plain.size() == data.size() && !data.empty())
		CHECK(memcmp(&plain[0], data.data(), data.size()) == 0);

	if (expect_smaller) CHECK(wire.size() < data.size());

	printf("  ok %-26s compressed %8zu -> %8zu bytes (ratio %.3f)\n",
		label, data.size(), wire.size(), ratio);
}

int main(int argc, char** argv)
{
	setbuf(stdout, 0);

	const char* dir = (argc > 1) ? argv[1] : "/tmp";
	testfile_path = std::string(dir) + "/quickdc_zupload_test.dat";

	/* Same bring-up order as QuickDC::Core; a Socket needs all of these. */
	Samurai::IO::Net::SocketMonitor::getInstance();
	new Samurai::IO::Net::BandwidthManager();
	Samurai::MessageHandler::getInstance();
	Samurai::TimerManager* timers = new Samurai::TimerManager();

	/* Highly compressible: the output is a fraction of the input, which is
	   where advancing the file position by bytes-written truncated the file. */
	test_plain("compressible", std::string(300000, 'A'));
	test_compressed("compressible", std::string(300000, 'A'), true);

	/* Incompressible: deflate briefly emits more than it consumes. */
	{
		std::string random;
		random.reserve(200000);
		unsigned int seed = 12345;
		for (size_t n = 0; n < 200000; n++)
		{
			seed = seed * 1103515245u + 12345u;
			random.push_back((char) ((seed >> 16) & 0xff));
		}
		test_plain("random", random);
		test_compressed("random", random, false);
	}

	/* Sizes around the buffer boundaries. */
	test_compressed("exactly one sndbuf", std::string(SMALL_SNDBUF, 'x'), true);
	test_compressed("one byte", std::string(1, 'Z'), false);
	test_compressed("single block", std::string(1000, 'q'), true);

	delete timers;

	Samurai::IO::File(testfile_path).remove();

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
