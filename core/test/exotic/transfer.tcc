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

#include <samurai/eventloop.h>
#include <samurai/io/file.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/inetaddress.h>

#include <samurai/io/net/socketglue.h>
#include <samurai/posix.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <memory>
#include <string>

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
		Listener() : stopped(false), last(0) { }
		void onTransferStopped(QuickDC::Transfer* which) override { stopped = true; last = which; }
		bool stopped;

		/** Which transfer the manager named, so a test can check it is the right one. */
		QuickDC::Transfer* last;
};

/*
 * Named by process as well as by counter, because each case here runs as a
 * process of its own and a static counter starts again at zero in every one of
 * them - which under a parallel run is two cases sharing a path.
 */
static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_transfer_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
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

static std::unique_ptr<Samurai::EventLoop> loop;

EXO_SETUP({
	loop = std::make_unique<Samurai::EventLoop>();
	return loop->isValid() ? 1 : 0;
});

EXO_TEARDOWN({
	loop.reset();
	return 1;
});

/*
 * A connected pair with one end owned by a Samurai socket and the other left as
 * a raw descriptor for the test to play the peer with.
 */
class Pair
{
	public:
		Pair() : peer(INVALID_SOCKET)
		{
			socket_t fds[2];
			if (!Samurai::IO::Net::socket_pair(fds)) return;

			Samurai::IO::Net::set_nonblocking(fds[0]);
			Samurai::IO::Net::set_nonblocking(fds[1]);

			int sndbuf = SMALL_SNDBUF;
			Samurai::IO::Net::set_sockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

			Samurai::IO::Net::InetSocketAddress addr("127.0.0.1", 0,
				Samurai::IO::Net::InetAddress::Version::IPv4);
			sock = Samurai::IO::Net::Socket::create(*loop, fds[0], addr);
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
			if (peer != INVALID_SOCKET) { Samurai::IO::Net::socket_close(peer); peer = INVALID_SOCKET; }
		}

		/* Whatever the peer can see right now. */
		void drain(std::string& out)
		{
			if (peer == INVALID_SOCKET) return;
			char buf[4096];
			ssize_t got;
			while ((got = Samurai::IO::Net::socket_recv(peer, buf, sizeof(buf))) > 0)
				out.append(buf, got);
		}

		bool send(const std::string& data)
		{
			size_t sent = 0;
			while (sent < data.size())
			{
				ssize_t out = Samurai::IO::Net::socket_send(peer, &data[sent], data.size() - sent);
				if (out > 0) { sent += (size_t) out; continue; }
				return false;
			}
			return true;
		}

		std::shared_ptr<Samurai::IO::Net::Socket> sock;
		socket_t peer;
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
			ssize_t out = Samurai::IO::Net::socket_send(pair.peer, &payload[sent], take);
			if (out > 0) sent += (size_t) out;
		}
		done = transfer->exec();
	}
	return done;
}

/* The whole file, as a baseline for the offset cases below. */

namespace {

/* The bytes every case below transfers. Built here rather than per case because
   it is the same twenty thousand bytes each time. */
const std::string data = pattern(20000);

}

EXO_TEST(transfer_a_whole_file_upload_sends_the_file,
{
		const std::string path = scratch("whole");
		writeFile(path, data);

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		std::string got;
		EXO_ASSERT(runUpload(&up, pair, got));
		EXO_ASSERT(got == data);
		EXO_ASSERT(up.isFinished());
		EXO_ASSERT(up.getTransferred() == data.size());
		EXO_ASSERT(up.getStatus() == QuickDC::Transfer::OK);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		EXO_ASSERT(listener.stopped);
		Samurai::IO::File(path).remove();
	
	return 1;
});

/* Resuming: the peer already has the first part and asks for the rest. */
EXO_TEST(transfer_an_upload_resumed_at_sends_only_the_tail,
{
		const std::string path = scratch("resume");
		writeFile(path, data);

		const off_t offset = 7777;

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file,
			offset, (off_t) (data.size() - offset));

		std::string got;
		EXO_ASSERT(runUpload(&up, pair, got));

		/* Exactly the tail, starting where it was asked to start. */
		EXO_ASSERT(got == data.substr(offset));
		EXO_ASSERT(up.isFinished());
		EXO_ASSERT(up.getOffset() == data.size());
		EXO_ASSERT(up.getTransferred() == data.size() - offset);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		EXO_ASSERT(listener.stopped);
		Samurai::IO::File(path).remove();
	
	return 1;
});

/* A chunk out of the middle stops at the length asked for. */
EXO_TEST(transfer_an_upload_of_a_middle_chunk_sends_that_chunk_and_stops,
{
		const std::string path = scratch("chunk");
		writeFile(path, data);

		const off_t offset = 4096;
		const off_t length = 3000;

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, offset, length);

		std::string got;
		EXO_ASSERT(runUpload(&up, pair, got));
		EXO_ASSERT(got == data.substr(offset, length));
		EXO_ASSERT(up.getTransferred() == (uint64_t) length);
		EXO_ASSERT(up.getTotal() == (uint64_t) (offset + length));

		manager.process();
		Samurai::IO::File(path).remove();
	
	return 1;
});

/* An upload of nothing is complete before it starts. */
EXO_TEST(transfer_an_upload_with_nothing_left_to_send_finishes_immediately,
{
		const std::string path = scratch("empty");
		writeFile(path, data);

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, (off_t) data.size(), 0);

		std::string got;
		EXO_ASSERT(runUpload(&up, pair, got));
		EXO_ASSERT(got.empty());
		EXO_ASSERT(up.isFinished());

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		EXO_ASSERT(listener.stopped);
		Samurai::IO::File(path).remove();
	
	return 1;
});

/*
	 * The peer goes away half way through. The write fails, and 'offset' used
	 * to be advanced by the -1 that failure returns: an unsigned subtraction
	 * that put the transfer one byte further from the end than it started, so
	 * it could never finish. Nothing else stops it either - the socket is dead,
	 * so every later write reports 0 - and the transfer stays in the manager
	 * with its session never told, holding the connection open for good.
	 */
EXO_TEST(transfer_an_upload_whose_peer_vanishes_stops_instead_of_spinning,
{
		const std::string path = scratch("vanish");
		writeFile(path, data);

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		/* One chunk out, then the peer is gone. */
		EXO_ASSERT(!up.exec());
		std::string got;
		pair.drain(got);
		pair.closePeer();

		int spins = 0;
		bool done = false;
		while (!done && spins++ < MAX_SPINS)
			done = up.exec();

		EXO_ASSERT(done);
		EXO_ASSERT(!up.isFinished());
		EXO_ASSERT(up.getStatus() != QuickDC::Transfer::OK);

		/* Never past the end, whichever way the arithmetic went. */
		EXO_ASSERT(up.getOffset() <= up.getTotal());

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		EXO_ASSERT(listener.stopped);
		Samurai::IO::File(path).remove();
	
	return 1;
});

/*
	 * The file shrinks under us. The read comes back short of the chunk we
	 * asked for, and eventually empty; writing nothing does not advance the
	 * offset, so without noticing this the transfer polls forever.
	 */
EXO_TEST(transfer_an_upload_of_a_file_that_shrinks_stops_instead_of_spinning,
{
		const std::string path = scratch("shrink");
		writeFile(path, data);

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		EXO_ASSERT(!up.exec());
		std::string got;
		pair.drain(got);

		EXO_ASSERT(truncate(path.c_str(), 1024) == 0);

		int spins = 0;
		bool done = false;
		while (!done && spins++ < MAX_SPINS)
		{
			done = up.exec();
			pair.drain(got);
		}

		EXO_ASSERT(done);
		EXO_ASSERT(up.getStatus() != QuickDC::Transfer::OK);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		EXO_ASSERT(listener.stopped);
		Samurai::IO::File(path).remove();
	
	return 1;
});

/*
	 * A file that cannot be opened at all. Upload only logged that, and exec()
	 * then returned 'not finished' on every call for a file it was never going
	 * to read, so this transfer too was polled for the lifetime of the process.
	 */
EXO_TEST(transfer_an_upload_of_a_file_that_will_not_open_gives_up,
{
		const std::string path = scratch("nosuchdir") + "/file";

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload up(&manager, &listener, pair.sock, &file, 0, 1024);

		EXO_ASSERT(up.exec());
		EXO_ASSERT(up.getStatus() != QuickDC::Transfer::OK);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		EXO_ASSERT(listener.stopped);
	
	return 1;
});

/* A download into a file that is not there yet. */
EXO_TEST(transfer_a_download_writes_the_file_it_was_asked_for,
{
		const std::string path = scratch("dl-new");

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		EXO_ASSERT(runDownload(&down, pair, data));
		EXO_ASSERT(down.isFinished());
		EXO_ASSERT(down.getTransferred() == data.size());
		EXO_ASSERT(down.getStatus() == QuickDC::Transfer::OK);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		EXO_ASSERT(listener.stopped);
		EXO_ASSERT(readFile(path) == data);
		Samurai::IO::File(path).remove();
	
	return 1;
});

/*
	 * Resuming a download. The part already on disk has to survive - the file
	 * is opened for writing without truncating precisely because it may hold
	 * chunks fetched from other sources.
	 */
EXO_TEST(transfer_a_resumed_download_keeps_what_was_already_on_disk,
{
		const std::string path = scratch("dl-resume");

		const size_t offset = 8192;
		writeFile(path, data.substr(0, offset));

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file,
			(off_t) offset, (off_t) (data.size() - offset));

		EXO_ASSERT(runDownload(&down, pair, data.substr(offset)));
		EXO_ASSERT(down.isFinished());
		EXO_ASSERT(down.getTransferred() == data.size() - offset);

		manager.process();

		/* The whole file, not just the part that arrived now. */
		EXO_ASSERT(readFile(path) == data);
		Samurai::IO::File(path).remove();
	
	return 1;
});

/*
	 * A chunk out of the middle of a file that is already complete around it,
	 * as the queue asks for when it fills a hole. Nothing outside the chunk may
	 * be disturbed, and the file must not grow.
	 */
EXO_TEST(transfer_a_download_filling_a_hole_leaves_the_rest_of_the_file,
{
		const std::string path = scratch("dl-hole");

		const size_t offset = 5000;
		const size_t length = 2000;

		std::string onDisk = data;
		for (size_t n = offset; n < offset + length; n++) onDisk[n] = '.';
		writeFile(path, onDisk);

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file,
			(off_t) offset, (off_t) length);

		/* More is offered than was asked for; only the chunk may be taken. */
		EXO_ASSERT(runDownload(&down, pair, data.substr(offset)));
		EXO_ASSERT(down.isFinished());
		EXO_ASSERT(down.getTransferred() == length);

		manager.process();
		EXO_ASSERT(readFile(path) == data);
		Samurai::IO::File(path).remove();
	
	return 1;
});

/*
	 * The command parser reads by the bufferful, so the first bytes of the file
	 * usually arrive along with the command that announced it. They are handed
	 * over rather than lost, and have to be written before anything read later.
	 */
EXO_TEST(transfer_bytes_read_ahead_by_the_parser_are_written_before_the_rest,
{
		const std::string path = scratch("dl-pending");

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		const size_t head = 3000;
		down.pushPending(data.data(), head);

		EXO_ASSERT(runDownload(&down, pair, data.substr(head)));
		EXO_ASSERT(down.isFinished());
		EXO_ASSERT(down.getTransferred() == data.size());

		manager.process();
		EXO_ASSERT(readFile(path) == data);
		Samurai::IO::File(path).remove();
	
	return 1;
});

/* Handed over more than the chunk covers, the surplus is not written. */
EXO_TEST(transfer_a_chunk_shorter_than_the_bytes_read_ahead_stops_at_its,
{
		const std::string path = scratch("dl-pending-over");

		const size_t length = 1000;

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file, 0, (off_t) length);
		down.pushPending(data.data(), 4096);

		int spins = 0;
		bool done = false;
		while (!done && spins++ < MAX_SPINS) done = down.exec();

		EXO_ASSERT(done);
		EXO_ASSERT(down.isFinished());
		EXO_ASSERT(down.getTransferred() == length);

		manager.process();
		EXO_ASSERT(readFile(path) == data.substr(0, length));
		Samurai::IO::File(path).remove();
	
	return 1;
});

/* The peer closes before the chunk is complete. */
EXO_TEST(transfer_a_download_cut_short_stops_and_keeps_what_arrived,
{
		const std::string path = scratch("dl-short");

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file, 0, (off_t) data.size());

		EXO_ASSERT(pair.send(data.substr(0, 1024)));
		pair.closePeer();

		int spins = 0;
		bool done = false;
		while (!done && spins++ < MAX_SPINS) done = down.exec();

		EXO_ASSERT(done);
		EXO_ASSERT(!down.isFinished());
		EXO_ASSERT(down.getStatus() != QuickDC::Transfer::OK);

		/* What did arrive is kept, so it can be resumed from there. */
		EXO_ASSERT(down.getTransferred() == 1024);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		EXO_ASSERT(listener.stopped);
		Samurai::IO::File(path).remove();
	
	return 1;
});

/* Somewhere that cannot be written. */
EXO_TEST(transfer_a_download_that_cannot_open_its_file_gives_up,
{
		const std::string path = scratch("nosuchdir") + "/file";

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Download down(&manager, &listener, pair.sock, &file, 0, 1024);

		EXO_ASSERT(down.exec());
		EXO_ASSERT(down.getStatus() != QuickDC::Transfer::OK);

		/* The manager defers the notification, so run it as the loop would. */
		manager.process();
		EXO_ASSERT(listener.stopped);
	
	return 1;
});

/*
	 * A transfer released before its announcement is drained.
	 *
	 * The session is told from process() and it is the session that then deletes
	 * the transfer, but not every deletion arrives that way round: a connection
	 * dropping in the same turn as a chunk finished has the session release the
	 * transfer on the spot. The entry waiting in the manager then named memory
	 * that had already been freed, and the next process() read it - which with a
	 * file arriving in chunks is a window that comes round several times a
	 * second.
	 */
EXO_TEST(transfer_a_transfer_released_before_the_drain_is_not_announced,
{
		const std::string path = scratch("released-early");
		writeFile(path, data);

		Pair pair;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload* up = new QuickDC::Upload(&manager, &listener, pair.sock,
			&file, 0, (off_t) data.size());

		std::string got;
		EXO_ASSERT(runUpload(up, pair, got));
		EXO_ASSERT(up->isFinished());

		/* Stopped, and waiting to be announced. */
		EXO_ASSERT(!listener.stopped);
		EXO_ASSERT(manager.size() == 1);

		delete up;
		EXO_ASSERT(manager.size() == 0);

		/* Nothing is left naming it, so there is nothing to announce. */
		manager.process();
		EXO_ASSERT(!listener.stopped);

		Samurai::IO::File(path).remove();
	
	return 1;
});

/*
	 * Two transfers, one listener.
	 *
	 * That is what a connection has whenever it holds more than one over its life -
	 * a peer asking us for a file while we are fetching one from them is enough. The
	 * notification used to carry nothing, so the listener released whichever transfer
	 * it happened to be holding: for the one that stopped that was right, and for
	 * anything else it deleted an object the socket was still feeding.
	 */
EXO_TEST(transfer_the_listener_is_told_which_of_its_transfers_stopped,
{
		const std::string path = scratch("two-at-once");
		writeFile(path, data);

		Pair one, two;
		QuickDC::TransferManager manager(*loop);
		Listener listener;
		Samurai::IO::File file(path);

		QuickDC::Upload first(&manager, &listener, one.sock, &file,
			0, (off_t) data.size());
		QuickDC::Upload second(&manager, &listener, two.sock, &file,
			0, (off_t) data.size());

		EXO_ASSERT(manager.size() == 2);

		/* The second runs out; the first is left alone half way through. */
		std::string got;
		EXO_ASSERT(runUpload(&second, two, got));
		EXO_ASSERT(second.isFinished());
		EXO_ASSERT(!first.isFinished());

		manager.process();

		EXO_ASSERT(listener.stopped);
		EXO_ASSERT(listener.last == &second);

		/* And the one still running is still there. */
		EXO_ASSERT(manager.size() == 2);

		Samurai::IO::File(path).remove();
	
	return 1;
});
