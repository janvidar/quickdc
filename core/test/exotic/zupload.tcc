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
 *
 * Each case prints what it moved and, for a compressed one, the ratio it got.
 * That line is a measurement rather than an assertion: how well deflate does on
 * a given payload is zlib's business and no test here scores it. The one thing
 * asserted about size is direction - a payload that must compress does, and one
 * that cannot is not required to.
 */

#include "quickdc.h"
#include "network/transfer.h"
#include "network/transfermanager.h"

#include <samurai/eventloop.h>
#include <samurai/io/file.h>
#include <samurai/io/compression.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/inetaddress.h>

#include <samurai/io/net/socketglue.h>
#include <samurai/posix.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

#define SMALL_SNDBUF 2048

class Listener : public QuickDC::PrivateTransferListener
{
	public:
		Listener() : stopped(false), last(0) { }
		void onTransferStopped(QuickDC::Transfer* which) override { stopped = true; last = which; }
		bool stopped;

		/** Which transfer the manager named, so a test can check it is the right one. */
		QuickDC::Transfer* last;
};

static std::string testfile_path;

static std::unique_ptr<Samurai::EventLoop> loop;

static bool write_test_file(const std::string& data)
{
	Samurai::IO::File file(testfile_path);
	if (!file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate)) return false;

	bool ok = (file.write(data.data(), data.size()) == (ssize_t) data.size());
	file.close();
	return ok;
}

/**
 * What a case can come to. A socketpair this host will not give us is not the
 * same answer as a transfer that ran and got it wrong, so the two are told
 * apart rather than both reported as a failure.
 */
enum class Outcome { Ok, Failed, Unsupported };

/**
 * Run a transfer to completion, draining the far end as it goes.
 *
 * @param compress use ZUpload rather than Upload.
 * @param out      everything that arrived on the far end.
 * @param unsupported set when this host would not give us a socket pair.
 * @return false if the transfer never finished.
 */
static bool run_transfer(bool compress, const std::string& data, std::vector<char>& out,
	double& ratio, bool& unsupported)
{
	unsupported = false;

	socket_t fds[2];
	if (!Samurai::IO::Net::socket_pair(fds))
	{
		unsupported = true;
		return false;
	}

	/* Non-blocking, so a full send buffer reports EAGAIN instead of waiting. */
	Samurai::IO::Net::set_nonblocking(fds[0]);
	Samurai::IO::Net::set_nonblocking(fds[1]);

	int sndbuf = SMALL_SNDBUF;
	Samurai::IO::Net::set_sockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

	Samurai::IO::Net::InetSocketAddress addr("127.0.0.1", 0, Samurai::IO::Net::InetAddress::Version::IPv4);
	auto sock = Samurai::IO::Net::Socket::create(*loop, fds[0], addr);

	QuickDC::TransferManager manager(*loop);
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
		while ((got = Samurai::IO::Net::socket_recv(fds[1], buf, sizeof(buf))) > 0)
			out.insert(out.end(), buf, buf + got);
	}

	/* The last write may still be sitting in the socket buffer. */
	char buf[4096];
	ssize_t got;
	while ((got = Samurai::IO::Net::socket_recv(fds[1], buf, sizeof(buf))) > 0)
		out.insert(out.end(), buf, buf + got);

	ratio = transfer->getRatio();
	bool complete = transfer->isFinished() && finished;

	/* The manager defers this, so run it as the message loop would. */
	manager.process();

	delete transfer;
	sock.reset();
	Samurai::IO::Net::socket_close(fds[1]);

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

/*
 * The two checks, as bool-returning helpers that say what went wrong rather
 * than merely that something did - the caller has only the label to go on
 * otherwise. Callers turn Failed into an assertion and Unsupported into a skip.
 */
static Outcome test_plain(const char* label, const std::string& data)
{
	if (!write_test_file(data))
	{
		printf("    %s: the payload could not be written to %s\n",
			label, testfile_path.c_str());
		return Outcome::Failed;
	}

	std::vector<char> wire;
	double ratio = 0.0;
	bool unsupported = false;
	if (!run_transfer(false, data, wire, ratio, unsupported))
	{
		if (unsupported) return Outcome::Unsupported;
		printf("    %s: plain transfer did not finish\n", label);
		return Outcome::Failed;
	}

	/* An uncompressed upload has to be byte for byte what is on disk. */
	if (wire.size() != data.size())
	{
		printf("    %s: %zu bytes on the wire from %zu on disk\n",
			label, wire.size(), data.size());
		return Outcome::Failed;
	}

	if (!data.empty() && memcmp(&wire[0], data.data(), data.size()) != 0)
	{
		printf("    %s: the bytes on the wire are not the bytes on disk\n", label);
		return Outcome::Failed;
	}

	printf("    %-26s plain      %8zu -> %8zu bytes\n", label, data.size(), wire.size());
	return Outcome::Ok;
}

static Outcome test_compressed(const char* label, const std::string& data, bool expect_smaller)
{
	if (!write_test_file(data))
	{
		printf("    %s: the payload could not be written to %s\n",
			label, testfile_path.c_str());
		return Outcome::Failed;
	}

	std::vector<char> wire;
	double ratio = 0.0;
	bool unsupported = false;
	if (!run_transfer(true, data, wire, ratio, unsupported))
	{
		if (unsupported) return Outcome::Unsupported;
		printf("    %s: compressed transfer did not finish\n", label);
		return Outcome::Failed;
	}

	/*
	 * The stream has to be complete. Stopping when the file position reaches
	 * the end truncates it, because deflate still owes us the flush.
	 */
	std::vector<char> plain;
	if (!inflate_all(wire, plain))
	{
		printf("    %s: the compressed stream does not inflate\n", label);
		return Outcome::Failed;
	}

	if (plain.size() != data.size())
	{
		printf("    %s: %zu bytes inflated from a %zu byte payload\n",
			label, plain.size(), data.size());
		return Outcome::Failed;
	}

	if (!data.empty() && memcmp(&plain[0], data.data(), data.size()) != 0)
	{
		printf("    %s: what inflated is not what was sent\n", label);
		return Outcome::Failed;
	}

	if (expect_smaller && !(wire.size() < data.size()))
	{
		printf("    %s: %zu bytes on the wire from a %zu byte payload, which should compress\n",
			label, wire.size(), data.size());
		return Outcome::Failed;
	}

	printf("    %-26s compressed %8zu -> %8zu bytes (ratio %.3f)\n",
		label, data.size(), wire.size(), ratio);
	return Outcome::Ok;
}

/* Incompressible, and the same bytes every run so a failure can be repeated. */
static std::string pseudorandom(size_t size)
{
	std::string out;
	out.reserve(size);
	unsigned int seed = 12345;
	for (size_t n = 0; n < size; n++)
	{
		seed = seed * 1103515245u + 12345u;
		out.push_back((char) ((seed >> 16) & 0xff));
	}
	return out;
}

EXO_SETUP({
	/*
	 * The path main() took from argv[1] so the payload could be put on another
	 * filesystem by hand. There is nowhere to pass that now, and nothing here
	 * depends on which filesystem it is, so it is /tmp - unique per process,
	 * because ctest may be running the cases beside each other.
	 */
	testfile_path = "/tmp/quickdc_zupload_" + std::to_string((int) getpid()) + ".dat";

	loop = std::make_unique<Samurai::EventLoop>();
	return loop->isValid() ? 1 : 0;
});

EXO_TEARDOWN({
	loop.reset();

	Samurai::IO::File(testfile_path).remove();
	return 1;
});

/* ------------------------------------------------------------------------- */

/* Highly compressible: the output is a fraction of the input, which is
   where advancing the file position by bytes-written truncated the file. */
EXO_TEST(zupload_compressible_plain,
{
	const Outcome got = test_plain("compressible", std::string(300000, 'A'));
	if (got == Outcome::Unsupported) EXO_SKIP("this host will not give us a socket pair");
	EXO_ASSERT(got == Outcome::Ok);
	return 1;
});

EXO_TEST(zupload_compressible_compressed,
{
	const Outcome got = test_compressed("compressible", std::string(300000, 'A'), true);
	if (got == Outcome::Unsupported) EXO_SKIP("this host will not give us a socket pair");
	EXO_ASSERT(got == Outcome::Ok);
	return 1;
});

/* Incompressible: deflate briefly emits more than it consumes. */
EXO_TEST(zupload_random_plain,
{
	const Outcome got = test_plain("random", pseudorandom(200000));
	if (got == Outcome::Unsupported) EXO_SKIP("this host will not give us a socket pair");
	EXO_ASSERT(got == Outcome::Ok);
	return 1;
});

EXO_TEST(zupload_random_compressed,
{
	const Outcome got = test_compressed("random", pseudorandom(200000), false);
	if (got == Outcome::Unsupported) EXO_SKIP("this host will not give us a socket pair");
	EXO_ASSERT(got == Outcome::Ok);
	return 1;
});

/* Sizes around the buffer boundaries. */
EXO_TEST(zupload_exactly_one_sndbuf_compressed,
{
	const Outcome got =
		test_compressed("exactly one sndbuf", std::string(SMALL_SNDBUF, 'x'), true);
	if (got == Outcome::Unsupported) EXO_SKIP("this host will not give us a socket pair");
	EXO_ASSERT(got == Outcome::Ok);
	return 1;
});

EXO_TEST(zupload_one_byte_compressed,
{
	const Outcome got = test_compressed("one byte", std::string(1, 'Z'), false);
	if (got == Outcome::Unsupported) EXO_SKIP("this host will not give us a socket pair");
	EXO_ASSERT(got == Outcome::Ok);
	return 1;
});

EXO_TEST(zupload_single_block_compressed,
{
	const Outcome got = test_compressed("single block", std::string(1000, 'q'), true);
	if (got == Outcome::Unsupported) EXO_SKIP("this host will not give us a socket pair");
	EXO_ASSERT(got == Outcome::Ok);
	return 1;
});
