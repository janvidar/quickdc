/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <algorithm>
#include <span>
#include <samurai/io/net/socket.h>
#include <samurai/io/file.h>

#include "network/clientsession.h"
#include "network/transfer.h"
#include "network/transfermanager.h"
#include "network/dc/dcconnection.h"

QuickDC::Transfer::Transfer(TransferManager* mgr, PrivateTransferListener* listener_, std::shared_ptr<Samurai::IO::Net::Socket> sock, Samurai::IO::File* file_, Direction dir_, off_t offset_, off_t length_)
	: manager(mgr), listener(listener_), socket(std::move(sock)), file(file_), direction(dir_), ratio(1.0)
	, status(OK), closed(false), offset(offset_), length(length_)
{
	manager->add(this);

	total = offset + length;
	QDBG("===> Transfer, offset=%d, length=%d (total=%d)", static_cast<int>(offset), static_cast<int>(length), static_cast<int>(total));
}

QuickDC::Transfer::~Transfer()
{
	manager->remove(this);
}

bool QuickDC::Transfer::exec()
{
	QDBG("QuickDC::Transfer::exec()");

	/* A socket can still report readable after we are done with it. */
	if (closed) return true;

	/*
	 * Nobody is going to open it later, so this is the end of the transfer and
	 * not something to come back to. Reporting 'not finished' left it in the
	 * manager being polled, with its session never told, for good.
	 */
	if (!file->isOpen())
	{
		QERR("QuickDC::Transfer::exec(): file is not open!");
		status = IOFail;
		closed = true;
		close();
		return true;
	}

	transfer();

	/*
	 * Stop on failure as well as on completion. Without the status check a
	 * download whose peer vanished mid-chunk would never reach 'total', and
	 * would sit here being polled forever.
	 */
	if (status != OK || isTransferComplete())
	{
		closed = true;
		close();
		return true;
	}
	return false;
}


#define MAXSENDBUF 1024*1024*4

QuickDC::Upload::Upload(TransferManager* mgr, PrivateTransferListener* listener, std::shared_ptr<Samurai::IO::Net::Socket> conn, Samurai::IO::File* file_, off_t offset, off_t length)
	: QuickDC::Transfer(mgr, listener, std::move(conn), file_, QuickDC::Transfer::Upload, offset, length)
{
	QDBG("QuickDC::Upload::Upload()");

	if (!file->open(Samurai::IO::File::Mode::Read))
	{
		QERR("Unable to open file: %s", file->getName().c_str());
		status = IOFail;
		return;
	}

	QDBG("Upload-ctor: Seek to %d\n", static_cast<int>(offset));
	if (!file->seek(offset))
	{
		QERR("Unable to seek to %d in '%s'", static_cast<int>(offset), file->getName().c_str());
		status = IOFail;
		return;
	}

	manager->onTransferStarted(this);
	socket->toggleWriteNotifier(true);
	
	uploadBuffer.resize(std::min<size_t>(socket->getSendBufferSize(), MAXSENDBUF));
}

QuickDC::Upload::~Upload()
{
	socket->toggleWriteNotifier(false);

	QDBG("QuickDC::Upload::~Upload()");
	file->close();
}

void QuickDC::Upload::close()
{
		socket->toggleWriteNotifier(false);
		file->close();
		manager->onTransferStopped(this);
		QDBG("Upload finished.");
}


void QuickDC::Upload::transfer()
{
	QDBG("Upload: Seek to %d\n", static_cast<int>(offset));
	if (!file->seek(offset))
	{
		QERR("Upload: unable to seek to %d in '%s'", static_cast<int>(offset), file->getName().c_str());
		status = IOFail;
		return;
	}

	size_t chunk = static_cast<size_t>(std::min<uint64_t>(uploadBuffer.size(), total - offset));
	if (!chunk) return;

	/*
	 * A failing read hands back -1, and passing that on as a length reads a
	 * gigabyte and a half past the buffer; an empty one means the file shrank
	 * under us, and there is no way to send bytes that are no longer there.
	 */
	ssize_t in = file->read(uploadBuffer.data(), chunk);
	if (in <= 0)
	{
		QERR("Upload: unable to read from '%s'", file->getName().c_str());
		status = IOFail;
		return;
	}

	/*
	 * And a failing write hands back -1 too. Added to an unsigned offset that
	 * moved the transfer one byte further from its end than it started, so it
	 * could never complete - and since a dead socket reports 0 from then on,
	 * nothing else ended it either.
	 */
	ssize_t out = socket->write(uploadBuffer.data(), static_cast<size_t>(in));
	if (out < 0)
	{
		QERR("Upload: unable to write to the peer");
		status = IOFail;
		return;
	}

	QDBG("Upload::transfer, in=%d, out=%d", static_cast<int>(in), static_cast<int>(out));
	rate.add(out);
	offset += static_cast<uint64_t>(out);
}


QuickDC::ZUpload::ZUpload(TransferManager* mgr, PrivateTransferListener* listener, std::shared_ptr<Samurai::IO::Net::Socket> conn, Samurai::IO::File* file_, off_t offset, off_t length)
	: QuickDC::Upload(mgr, listener, std::move(conn), file_, offset, length)
	, compressor(0)
	, pendingSize(0), pendingOffset(0)
	, finished(false), compressedBytes(0)
{
	compressor = new Samurai::IO::GzipCompressor();

	compressBuffer.resize(uploadBuffer.size() << 1);

	/*
	 * Deflate can momentarily produce more than it consumes on incompressible
	 * input, so give the output side room rather than losing bytes we would
	 * have no way to regenerate.
	 */
	pendingBuffer.resize(compressBuffer.size() + (compressBuffer.size() >> 1) + 1024);
}

QuickDC::ZUpload::~ZUpload()
{
	delete compressor;
}


void QuickDC::ZUpload::close()
{
	QDBG("ZUpload finished: %d bytes sent for %d bytes of file (ratio %.2f)",
		static_cast<int>(compressedBytes), static_cast<int>(offset - (total - length)), ratio);
	Upload::close();
}

bool QuickDC::ZUpload::isTransferComplete() const
{
	/*
	 * Consuming the last input byte is not the end: deflate still owes us the
	 * flush, and that output still has to reach the socket.
	 */
	return (finished && pendingOffset >= pendingSize);
}

bool QuickDC::ZUpload::flushPending()
{
	while (pendingOffset < pendingSize)
	{
		ssize_t out = socket->write(pendingBuffer.data() + pendingOffset, pendingSize - pendingOffset);

		if (out < 0)
		{
			QERR("ZUpload: unable to write to the peer");
			status = IOFail;
			return false;
		}

		/* Send buffer is full; the write notifier will bring us back. */
		if (!out) return false;

		rate.add(out);
		compressedBytes += static_cast<uint64_t>(out);
		pendingOffset += static_cast<size_t>(out);
	}

	pendingSize = 0;
	pendingOffset = 0;
	return true;
}

void QuickDC::ZUpload::transfer()
{
	/* Anything left over from last time goes first, in order. */
	if (!flushPending()) return;
	if (finished) return;

	size_t consumed = 0;

	if (offset < total)
	{
		if (!file->seek(offset))
		{
			QERR("ZUpload: unable to seek to %d in '%s'", static_cast<int>(offset), file->getName().c_str());
			status = IOFail;
			return;
		}

		ssize_t chunk = static_cast<ssize_t>(std::min<uint64_t>(compressBuffer.size(), total - offset));
		ssize_t in = file->read(compressBuffer.data(), static_cast<size_t>(chunk));
		if (in <= 0)
		{
			QERR("ZUpload: unable to read from '%s'", file->getName().c_str());
			status = IOFail;
			return;
		}
		consumed = static_cast<size_t>(in);
	}

	/* An empty input is what tells the codec to flush. */
	const Samurai::IO::Codec::Progress p = compressor->step(
		std::span<const char>(compressBuffer.data(), consumed),
		std::span<char>(pendingBuffer));

	if (p.status == Samurai::IO::Codec::Status::Error)
	{
		QERR("ZUpload: deflate failed");
		status = IOFail;
		return;
	}

	/* Only what the compressor actually took advances the file position. */
	offset += p.consumed;

	/* The flush is over when the codec says so, not when a call happens to
	   produce nothing - a flush too large for pendingBuffer takes several. */
	if (p.status == Samurai::IO::Codec::Status::StreamEnd)
		finished = true;

	const size_t produced = p.produced;
	pendingSize = produced;
	pendingOffset = 0;

	if (offset > (total - length))
		ratio = static_cast<double>(compressedBytes + produced) / static_cast<double>(offset - (total - length));

	QDBG("ZUpload::transfer, consumed=%d, produced=%d, at %d/%d",
		static_cast<int>(consumed), static_cast<int>(produced), static_cast<int>(offset), static_cast<int>(total));

	flushPending();
}



QuickDC::Download::Download(TransferManager* mgr, PrivateTransferListener* listener, std::shared_ptr<Samurai::IO::Net::Socket> conn, Samurai::IO::File* file_, off_t offset, off_t length)
	: QuickDC::Transfer(mgr, listener, std::move(conn), file_, QuickDC::Transfer::Download, offset, length)
	, pendingOffset(0)
{
	QDBG("QuickDC::Download::Download()");

	/*
	 * Write only, and deliberately without Truncate: we are filling in a
	 * partial file that may already hold chunks fetched from other sources.
	 * File::open() adds O_CREAT for a write-only open, so a brand new
	 * download does not need special casing.
	 */
	if (!file->open(Samurai::IO::File::Mode::Write))
	{
		QERR("Unable to open '%s' for writing", file->getName().c_str());
		status = IOFail;
		return;
	}

	QDBG("Download-ctor: Seek to %d\n", static_cast<int>(offset));
	if (!file->seek(offset))
	{
		QERR("Unable to seek to %d in '%s'", static_cast<int>(offset), file->getName().c_str());
		status = IOFail;
		return;
	}

	manager->onTransferStarted(this);

	size_t want = std::min<size_t>(socket->getReceiveBufferSize(), MAXSENDBUF);
	if (!want) want = 8192;
	downloadBuffer.resize(want);
}

QuickDC::Download::~Download() {
	QDBG("QuickDC::Download::~Download()");
	file->close();
}

void QuickDC::Download::pushPending(const char* data, size_t length)
{
	if (!data || !length) return;

	/* Only what has not reached the file yet is carried over, and it stays in
	   front of what has just arrived: the peer sent them in that order. */
	pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pendingOffset));
	pendingOffset = 0;

	pending.insert(pending.end(), data, data + length);
}

void QuickDC::Download::close()
{
	file->close();
	manager->onTransferStopped(this);
	QDBG("Download stopped (%d/%d bytes).", static_cast<int>(offset), static_cast<int>(total));
}

void QuickDC::Download::transfer()
{
	if (downloadBuffer.empty())
	{
		status = IOFail;
		return;
	}

	/* Never read past the end of the chunk we asked for. */
	size_t want = downloadBuffer.size();
	uint64_t remaining = total - offset;
	if (remaining < static_cast<uint64_t>(want)) want = static_cast<size_t>(remaining);
	if (!want) return;

	if (!pending.empty())
	{
		size_t take = pending.size() - pendingOffset;
		if (static_cast<uint64_t>(take) > remaining) take = static_cast<size_t>(remaining);

		ssize_t out = file->write(pending.data() + pendingOffset, take);
		if (out != static_cast<ssize_t>(take))
		{
			QERR("Download: wrote %d of %d buffered bytes to '%s'",
				static_cast<int>(out), static_cast<int>(take), file->getName().c_str());
			status = IOFail;
			return;
		}

		rate.add(out);
		offset += static_cast<uint64_t>(out);
		pendingOffset += take;

		if (pendingOffset >= pending.size())
		{
			pending.clear();
			pendingOffset = 0;
		}

		return;
	}

	/*
	 * The three ways a read can come back empty have to be told apart: a
	 * socket that merely has nothing right now must be waited for, while a
	 * closed or broken one is never going to produce the rest of this chunk
	 * and has to fail out instead of being polled forever.
	 */
	size_t in = 0;
	std::error_code ec;
	switch (socket->read(downloadBuffer.data(), want, in, ec))
	{
		case Samurai::IO::ReadResult::Ok:
			break;

		case Samurai::IO::ReadResult::WouldBlock:
			return;

		case Samurai::IO::ReadResult::EndOfFile:
			QDBG("Download: the peer closed after %d of %d bytes", static_cast<int>(offset), static_cast<int>(total));
			status = IOFail;
			return;

		case Samurai::IO::ReadResult::Error:
			QERR("Download: unable to read from the peer: %s", ec.message().c_str());
			status = IOFail;
			return;
	}

	ssize_t out = file->write(downloadBuffer.data(), in);
	if (out != static_cast<ssize_t>(in))
	{
		QERR("Download: wrote %d of %d bytes to '%s'",
			static_cast<int>(out), static_cast<int>(in), file->getName().c_str());
		status = IOFail;
		return;
	}

	rate.add(out);
	offset += static_cast<uint64_t>(out);
}










