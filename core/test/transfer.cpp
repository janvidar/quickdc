/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The resume paths: a transfer that does not start at byte zero, and does not
 * necessarily run to the end of the file either. Every request a peer makes
 * carries an offset and a length, and both halves of the bookkeeping - where to
 * seek, how much to move, when to stop - are derived from them.
 *
 * Downloads had nothing exercising them at all, uploads only the whole-file
 * case (see zupload.cpp). The interesting cases are the ones where the peer or
 * the file misbehaves half way: those decide whether a transfer stops, or sits
 * in the poll loop forever holding a connection nobody will ever close.
 */

#include "quickdc.h"
#include "network/transfer.h"
#include "network/transfermanager.h"

#include <samurai/io/file.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/inetaddress.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <string>

static int failures = 0;

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
			failures++; \
		} \
	} while (0)

/* Small enough that a write fails part way and has to be retried. */
#define SMALL_SNDBUF 2048

/* Enough spins to move the largest payload here through a 2k send buffer. */
#define MAX_SPINS 200000

class Listener : public QuickDC::PrivateTransferListener
{
	public:
		Listener() : stopped(false) { }
		void onTransferStopped() { stopped = true; }
		bool stopped;
};

static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_transfer_" + std::to_string(counter++) + "_" + leaf;
}

static bool writeFile(const std::string& path, const std::string& content)
{
	FILE* fh = fopen(path.c_str(), "wb");
	if (!fh) return false;
	const bool ok = fwrite(content.data(), 1, content.size(), fh) == content.size();
	fclose(fh);
	return ok;
}

static std::string readFile(const std::string& path)
{
	FILE* fh = fopen(path.c_str(), "rb");
	if (!fh) return std::string();

	std::string out;
	char buf[65536];
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), fh)) > 0) out.append(buf, got);
	fclose(fh);
	return out;
}

/* Recognisable at any offset, so a chunk landing in the wrong place shows. */
static std::string pattern(size_t size)
{
	std::string out;
	out.reserve(size);
	for (size_t n = 0; n < size; n++)
		out += (char) ('0' + (n % 10));
	return out;
}

/*
 * A connected pair with one end owned by a Samurai socket and the other left as
 * a raw descriptor for the test to play the peer with.
 */
class Pair
{
	public:
		Pair() : peer(-1)
		{
			int fds[2];
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return;

			fcntl(fds[0], F_SETFL, O_NONBLOCK);
			fcntl(fds[1], F_SETFL, O_NONBLOCK);

			int sndbuf = SMALL_SNDBUF;
			setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

			Samurai::IO::Net::InetSocketAddress addr("127.0.0.1", 0,
				Samurai::IO::Net::InetAddress::Version::IPv4);
			sock = Samurai::IO::Net::Socket::create(fds[0], addr);
			peer = fds[1];
		}

		~Pair()
		{
			sock.reset();
			closePeer();
		}

		Pair(const Pair&) = delete;
		Pair& operator=(const Pair&) = delete;

		void closePeer()
		{
			if (peer != -1) { ::close(peer); peer = -1; }
		}

		/* Whatever the peer can see right now. */
		void drain(std::string& out)
		{
			if (peer == -1) return;
			char buf[4096];
			ssize_t got;
			while ((got = ::read(peer, buf, sizeof(buf))) > 0)
				out.append(buf, got);
		}

		bool send(const std::string& data)
		{
			size_t sent = 0;
			while (sent < data.size())
			{
				ssize_t out = ::write(peer, &data[sent], data.size() - sent);
				if (out > 0) { sent += (size_t) out; continue; }
				if (out == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
				return false;
			}
			return true;
		}

		std::shared_ptr<Samurai::IO::Net::Socket> sock;
		int peer;
};

/*
 * Runs an upload to a standstill, collecting what the peer sees. Returns
 * whether exec() ever said it was done, rather than us giving up on it.
 */
static bool runUpload(QuickDC::Transfer* transfer, Pair& pair, std::string& received)
{
	int spins = 0;
	bool done = false;

	while (!done && spins++ < MAX_SPINS)
	{
		done = transfer->exec();
		pair.drain(received);
	}
	pair.drain(received);
	return done;
}

/*
 * Same, but feeding the socket rather than reading it: the peer hands over
 * 'payload' in small pieces as the download makes room for them.
 */
static bool runDownload(QuickDC::Transfer* transfer, Pair& pair, const std::string& payload)
{
	size_t sent = 0;
	int spins = 0;
	bool done = false;

	while (!done && spins++ < MAX_SPINS)
	{
		if (sent < payload.size())
		{
			const size_t take = std::min<size_t>(1024, payload.size() - sent);
			ssize_t out = ::write(pair.peer, &payload[sent], take);
			if (out > 0) sent += (size_t) out;
		}
		done = transfer->exec();
	}
	return done;
}

int main()
{
	setbuf(stdout, 0);

	const std::string data = pattern(20000);

	/* The whole file, as a baseline for the offset cases below. */
	{
		const std::string path = scratch("whole");
		writeFile(path, data);

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		std::string got;
		CHECK(runUpload(&up, pair, got));
		CHECK(got == data);
		CHECK(up.isFinished());
		CHECK(up.getTransferred() == data.size());
		CHECK(up.getStatus() == QuickDC::Transfer::OK);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		CHECK(listener.stopped);
		Samurai::IO::File(path).remove();
		printf("  ok a whole-file upload sends the file\n");
	}

	/* Resuming: the peer already has the first part and asks for the rest. */
	{
		const std::string path = scratch("resume");
		writeFile(path, data);

		const off_t offset = 7777;

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file,
			offset, (off_t) (data.size() - offset));

		std::string got;
		CHECK(runUpload(&up, pair, got));

		/* Exactly the tail, starting where it was asked to start. */
		CHECK(got == data.substr(offset));
		CHECK(up.isFinished());
		CHECK(up.getOffset() == data.size());
		CHECK(up.getTransferred() == data.size() - offset);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		CHECK(listener.stopped);
		Samurai::IO::File(path).remove();
		printf("  ok an upload resumed at %d sends only the tail\n", (int) offset);
	}

	/* A chunk out of the middle stops at the length asked for. */
	{
		const std::string path = scratch("chunk");
		writeFile(path, data);

		const off_t offset = 4096;
		const off_t length = 3000;

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, offset, length);

		std::string got;
		CHECK(runUpload(&up, pair, got));
		CHECK(got == data.substr(offset, length));
		CHECK(up.getTransferred() == (uint64_t) length);
		CHECK(up.getTotal() == (uint64_t) (offset + length));

		manager.process();
		Samurai::IO::File(path).remove();
		printf("  ok an upload of a middle chunk sends that chunk and stops\n");
	}

	/* An upload of nothing is complete before it starts. */
	{
		const std::string path = scratch("empty");
		writeFile(path, data);

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, (off_t) data.size(), 0);

		std::string got;
		CHECK(runUpload(&up, pair, got));
		CHECK(got.empty());
		CHECK(up.isFinished());

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		CHECK(listener.stopped);
		Samurai::IO::File(path).remove();
		printf("  ok an upload with nothing left to send finishes immediately\n");
	}

	/*
	 * The peer goes away half way through. The write fails, and 'offset' used
	 * to be advanced by the -1 that failure returns: an unsigned subtraction
	 * that put the transfer one byte further from the end than it started, so
	 * it could never finish. Nothing else stops it either - the socket is dead,
	 * so every later write reports 0 - and the transfer stays in the manager
	 * with its session never told, holding the connection open for good.
	 */
	{
		const std::string path = scratch("vanish");
		writeFile(path, data);

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		/* One chunk out, then the peer is gone. */
		CHECK(!up.exec());
		std::string got;
		pair.drain(got);
		pair.closePeer();

		int spins = 0;
		bool done = false;
		while (!done && spins++ < MAX_SPINS)
			done = up.exec();

		CHECK(done);
		CHECK(!up.isFinished());
		CHECK(up.getStatus() != QuickDC::Transfer::OK);

		/* Never past the end, whichever way the arithmetic went. */
		CHECK(up.getOffset() <= up.getTotal());

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		CHECK(listener.stopped);
		Samurai::IO::File(path).remove();
		printf("  ok an upload whose peer vanishes stops instead of spinning\n");
	}

	/*
	 * The file shrinks under us. The read comes back short of the chunk we
	 * asked for, and eventually empty; writing nothing does not advance the
	 * offset, so without noticing this the transfer polls forever.
	 */
	{
		const std::string path = scratch("shrink");
		writeFile(path, data);

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		CHECK(!up.exec());
		std::string got;
		pair.drain(got);

		CHECK(truncate(path.c_str(), 1024) == 0);

		int spins = 0;
		bool done = false;
		while (!done && spins++ < MAX_SPINS)
		{
			done = up.exec();
			pair.drain(got);
		}

		CHECK(done);
		CHECK(up.getStatus() != QuickDC::Transfer::OK);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		CHECK(listener.stopped);
		Samurai::IO::File(path).remove();
		printf("  ok an upload of a file that shrinks stops instead of spinning\n");
	}

	/*
	 * A file that cannot be opened at all. Upload only logged that, and exec()
	 * then returned 'not finished' on every call for a file it was never going
	 * to read, so this transfer too was polled for the lifetime of the process.
	 */
	{
		const std::string path = scratch("nosuchdir") + "/file";

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, 0, 1024);

		CHECK(up.exec());
		CHECK(up.getStatus() != QuickDC::Transfer::OK);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		CHECK(listener.stopped);
		printf("  ok an upload of a file that will not open gives up\n");
	}

	/* A download into a file that is not there yet. */
	{
		const std::string path = scratch("dl-new");

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		CHECK(runDownload(&down, pair, data));
		CHECK(down.isFinished());
		CHECK(down.getTransferred() == data.size());
		CHECK(down.getStatus() == QuickDC::Transfer::OK);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		CHECK(listener.stopped);
		CHECK(readFile(path) == data);
		Samurai::IO::File(path).remove();
		printf("  ok a download writes the file it was asked for\n");
	}

	/*
	 * Resuming a download. The part already on disk has to survive - the file
	 * is opened for writing without truncating precisely because it may hold
	 * chunks fetched from other sources.
	 */
	{
		const std::string path = scratch("dl-resume");

		const size_t offset = 8192;
		writeFile(path, data.substr(0, offset));

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file,
			(off_t) offset, (off_t) (data.size() - offset));

		CHECK(runDownload(&down, pair, data.substr(offset)));
		CHECK(down.isFinished());
		CHECK(down.getTransferred() == data.size() - offset);

		manager.process();

		/* The whole file, not just the part that arrived now. */
		CHECK(readFile(path) == data);
		Samurai::IO::File(path).remove();
		printf("  ok a resumed download keeps what was already on disk\n");
	}

	/*
	 * A chunk out of the middle of a file that is already complete around it,
	 * as the queue asks for when it fills a hole. Nothing outside the chunk may
	 * be disturbed, and the file must not grow.
	 */
	{
		const std::string path = scratch("dl-hole");

		const size_t offset = 5000;
		const size_t length = 2000;

		std::string onDisk = data;
		for (size_t n = offset; n < offset + length; n++) onDisk[n] = '.';
		writeFile(path, onDisk);

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file,
			(off_t) offset, (off_t) length);

		/* More is offered than was asked for; only the chunk may be taken. */
		CHECK(runDownload(&down, pair, data.substr(offset)));
		CHECK(down.isFinished());
		CHECK(down.getTransferred() == length);

		manager.process();
		CHECK(readFile(path) == data);
		Samurai::IO::File(path).remove();
		printf("  ok a download filling a hole leaves the rest of the file alone\n");
	}

	/*
	 * The command parser reads by the bufferful, so the first bytes of the file
	 * usually arrive along with the command that announced it. They are handed
	 * over rather than lost, and have to be written before anything read later.
	 */
	{
		const std::string path = scratch("dl-pending");

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		const size_t head = 3000;
		down.pushPending(data.data(), head);

		CHECK(runDownload(&down, pair, data.substr(head)));
		CHECK(down.isFinished());
		CHECK(down.getTransferred() == data.size());

		manager.process();
		CHECK(readFile(path) == data);
		Samurai::IO::File(path).remove();
		printf("  ok bytes read ahead by the parser are written before the rest\n");
	}

	/* Handed over more than the chunk covers, the surplus is not written. */
	{
		const std::string path = scratch("dl-pending-over");

		const size_t length = 1000;

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file, 0, (off_t) length);
		down.pushPending(data.data(), 4096);

		int spins = 0;
		bool done = false;
		while (!done && spins++ < MAX_SPINS) done = down.exec();

		CHECK(done);
		CHECK(down.isFinished());
		CHECK(down.getTransferred() == length);

		manager.process();
		CHECK(readFile(path) == data.substr(0, length));
		Samurai::IO::File(path).remove();
		printf("  ok a chunk shorter than the bytes read ahead stops at its length\n");
	}

	/* The peer closes before the chunk is complete. */
	{
		const std::string path = scratch("dl-short");

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		CHECK(pair.send(data.substr(0, 1024)));
		pair.closePeer();

		int spins = 0;
		bool done = false;
		while (!done && spins++ < MAX_SPINS) done = down.exec();

		CHECK(done);
		CHECK(!down.isFinished());
		CHECK(down.getStatus() != QuickDC::Transfer::OK);

		/* What did arrive is kept, so it can be resumed from there. */
		CHECK(down.getTransferred() == 1024);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		CHECK(listener.stopped);
		Samurai::IO::File(path).remove();
		printf("  ok a download cut short stops and keeps what arrived\n");
	}

	/* Somewhere that cannot be written. */
	{
		const std::string path = scratch("nosuchdir") + "/file";

		Pair pair;
		QuickDC::TransferManager manager;
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file, 0, 1024);

		CHECK(down.exec());
		CHECK(down.getStatus() != QuickDC::Transfer::OK);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		CHECK(listener.stopped);
		printf("  ok a download that cannot open its file gives up\n");
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
