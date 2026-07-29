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
	QDBG("===> Transfer, offset=%d, length=%d (total=%d)", (int) offset, (int) length, (int) total);
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
	, uploadBuffer(0), uploadBufferSize(0)
{
	QDBG("QuickDC::Upload::Upload()");

	if (!file->open(Samurai::IO::File::Mode::Read))
	{
		QERR("Unable to open file: %s", file->getName().c_str());
		status = IOFail;
		return;
	}

	QDBG("Upload-ctor: Seek to %d\n", (int) offset);
	if (!file->seek(offset))
	{
		QERR("Unable to seek to %d in '%s'", (int) offset, file->getName().c_str());
		status = IOFail;
		return;
	}

	manager->onTransferStarted(this);
	socket->toggleWriteNotifier(true);
	
	uploadBufferSize = std::min<size_t>(socket->getSendBufferSize(), MAXSENDBUF);
	uploadBuffer = new char[uploadBufferSize];
}

QuickDC::Upload::~Upload()
{
	socket->toggleWriteNotifier(false);
	
	QDBG("QuickDC::Upload::~Upload()");
	file->close();
	
	delete[] uploadBuffer;
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
	QDBG("Upload: Seek to %d\n", (int) offset);
	if (!file->seek(offset))
	{
		QERR("Upload: unable to seek to %d in '%s'", (int) offset, file->getName().c_str());
		status = IOFail;
		return;
	}

	size_t chunk = (size_t) std::min<uint64_t>(uploadBufferSize, total - offset);
	if (!chunk) return;

	/*
	 * A failing read hands back -1, and passing that on as a length reads a
	 * gigabyte and a half past the buffer; an empty one means the file shrank
	 * under us, and there is no way to send bytes that are no longer there.
	 */
	ssize_t in = file->read(uploadBuffer, chunk);
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
	ssize_t out = socket->write(uploadBuffer, (size_t) in);
	if (out < 0)
	{
		QERR("Upload: unable to write to the peer");
		status = IOFail;
		return;
	}

	QDBG("Upload::transfer, in=%d, out=%d", (int) in, (int) out);
	rate.add(out);
	offset += (uint64_t) out;
}


QuickDC::ZUpload::ZUpload(TransferManager* mgr, PrivateTransferListener* listener, std::shared_ptr<Samurai::IO::Net::Socket> conn, Samurai::IO::File* file_, off_t offset, off_t length)
	: QuickDC::Upload(mgr, listener, std::move(conn), file_, offset, length)
	, compressor(0)
	, compressBuffer(0), compressBufferSize(0)
	, pendingBuffer(0), pendingCapacity(0), pendingSize(0), pendingOffset(0)
	, finished(false), compressedBytes(0)
{
	compressor = new Samurai::IO::GzipCompressor();

	compressBufferSize = (uploadBufferSize << 1);
	compressBuffer = new char[compressBufferSize];

	/*
	 * Deflate can momentarily produce more than it consumes on incompressible
	 * input, so give the output side room rather than losing bytes we would
	 * have no way to regenerate.
	 */
	pendingCapacity = compressBufferSize + (compressBufferSize >> 1) + 1024;
	pendingBuffer = new char[pendingCapacity];
}

QuickDC::ZUpload::~ZUpload()
{
	delete compressor;
	delete[] compressBuffer;
	delete[] pendingBuffer;
}


void QuickDC::ZUpload::close()
{
	QDBG("ZUpload finished: %d bytes sent for %d bytes of file (ratio %.2f)",
		(int) compressedBytes, (int) (offset - (total - length)), ratio);
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
		ssize_t out = socket->write(&pendingBuffer[pendingOffset], pendingSize - pendingOffset);

		if (out < 0)
		{
			QERR("ZUpload: unable to write to the peer");
			status = IOFail;
			return false;
		}

		/* Send buffer is full; the write notifier will bring us back. */
		if (!out) return false;

		rate.add(out);
		compressedBytes += (uint64_t) out;
		pendingOffset += (size_t) out;
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
			QERR("ZUpload: unable to seek to %d in '%s'", (int) offset, file->getName().c_str());
			status = IOFail;
			return;
		}

		ssize_t chunk = (ssize_t) std::min<uint64_t>(compressBufferSize, total - offset);
		ssize_t in = file->read(compressBuffer, (size_t) chunk);
		if (in <= 0)
		{
			QERR("ZUpload: unable to read from '%s'", file->getName().c_str());
			status = IOFail;
			return;
		}
		consumed = (size_t) in;
	}

	/* An empty input is what tells the codec to flush. */
	const Samurai::IO::Codec::Progress p = compressor->step(
		std::span<const char>(compressBuffer, consumed),
		std::span<char>(pendingBuffer, pendingCapacity));

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
		ratio = (double) (compressedBytes + produced) / (double) (offset - (total - length));

	QDBG("ZUpload::transfer, consumed=%d, produced=%d, at %d/%d",
		(int) consumed, (int) produced, (int) offset, (int) total);

	flushPending();
}



QuickDC::Download::Download(TransferManager* mgr, PrivateTransferListener* listener, std::shared_ptr<Samurai::IO::Net::Socket> conn, Samurai::IO::File* file_, off_t offset, off_t length)
	: QuickDC::Transfer(mgr, listener, std::move(conn), file_, QuickDC::Transfer::Download, offset, length)
	, downloadBuffer(0), downloadBufferSize(0)
	, pending(0), pendingSize(0), pendingOffset(0)
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

	QDBG("Download-ctor: Seek to %d\n", (int) offset);
	if (!file->seek(offset))
	{
		QERR("Unable to seek to %d in '%s'", (int) offset, file->getName().c_str());
		status = IOFail;
		return;
	}

	manager->onTransferStarted(this);

	downloadBufferSize = std::min<size_t>(socket->getReceiveBufferSize(), MAXSENDBUF);
	if (!downloadBufferSize) downloadBufferSize = 8192;
	downloadBuffer = new char[downloadBufferSize];
}

QuickDC::Download::~Download() {
	QDBG("QuickDC::Download::~Download()");
	file->close();
	delete[] downloadBuffer;
	free(pending);
}

void QuickDC::Download::pushPending(const char* data, size_t length)
{
	if (!data || !length) return;

	size_t left = pendingSize - pendingOffset;
	char* buf = (char*) malloc(left + length);
	if (!buf)
	{
		QERR("Download: out of memory taking over %d buffered bytes", (int) length);
		status = IOFail;
		return;
	}

	if (left) memcpy(buf, &pending[pendingOffset], left);
	memcpy(&buf[left], data, length);

	free(pending);
	pending = buf;
	pendingSize = left + length;
	pendingOffset = 0;
}

void QuickDC::Download::close()
{
	file->close();
	manager->onTransferStopped(this);
	QDBG("Download stopped (%d/%d bytes).", (int) offset, (int) total);
}

void QuickDC::Download::transfer()
{
	if (!downloadBuffer)
	{
		status = IOFail;
		return;
	}

	/* Never read past the end of the chunk we asked for. */
	size_t want = downloadBufferSize;
	uint64_t remaining = total - offset;
	if (remaining < (uint64_t) want) want = (size_t) remaining;
	if (!want) return;

	if (pending)
	{
		size_t take = pendingSize - pendingOffset;
		if ((uint64_t) take > remaining) take = (size_t) remaining;

		ssize_t out = file->write(&pending[pendingOffset], take);
		if (out != (ssize_t) take)
		{
			QERR("Download: wrote %d of %d buffered bytes to '%s'",
				(int) out, (int) take, file->getName().c_str());
			status = IOFail;
			return;
		}

		rate.add(out);
		offset += (uint64_t) out;
		pendingOffset += take;

		if (pendingOffset >= pendingSize)
		{
			free(pending);
			pending = 0;
			pendingSize = 0;
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
	switch (socket->read(downloadBuffer, want, in, ec))
	{
		case Samurai::IO::ReadResult::Ok:
			break;

		case Samurai::IO::ReadResult::WouldBlock:
			return;

		case Samurai::IO::ReadResult::EndOfFile:
			QDBG("Download: the peer closed after %d of %d bytes", (int) offset, (int) total);
			status = IOFail;
			return;

		case Samurai::IO::ReadResult::Error:
			QERR("Download: unable to read from the peer: %s", ec.message().c_str());
			status = IOFail;
			return;
	}

	ssize_t out = file->write(downloadBuffer, in);
	if (out != (ssize_t) in)
	{
		QERR("Download: wrote %d of %d bytes to '%s'",
			(int) out, (int) in, file->getName().c_str());
		status = IOFail;
		return;
	}

	rate.add(out);
	offset += (uint64_t) out;
}










