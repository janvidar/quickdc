/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <samurai/io/net/socket.h>
#include <samurai/io/file.h>

#include "network/clientsession.h"
#include "network/transfer.h"
#include "network/transfermanager.h"
#include "network/dc/dcconnection.h"

QuickDC::Transfer::Transfer(TransferManager* mgr, PrivateTransferListener* listener_, Samurai::IO::Net::Socket* sock, Samurai::IO::File* file_, Direction dir_, off_t offset_, off_t length_)
	: manager(mgr), listener(listener_), socket(sock), file(file_), direction(dir_), ratio(1.0)
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

	if (!file->isOpen())
	{
		QERR("QuickDC::Transfer::exec(): file is not open!");
		return false;
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

QuickDC::Upload::Upload(TransferManager* mgr, PrivateTransferListener* listener, Samurai::IO::Net::Socket* conn, Samurai::IO::File* file_, off_t offset, off_t length)
	: QuickDC::Transfer(mgr, listener, conn, file_, QuickDC::Transfer::Upload, offset, length), uploadBuffer(0)
{
	QDBG("QuickDC::Upload::Upload()");
	
	bool ok = file->open(Samurai::IO::File::Read);
	if (!ok)
	{
		QERR("Unable to open file: %s", file->getName().c_str());
	}
	
	QDBG("Upload-ctor: Seek to %d\n", (int) offset);
	file->seek(offset);
	
	manager->onTransferStarted(this);
	socket->toggleWriteNotifier(true);
	
	uploadBufferSize = MIN(((size_t) socket->getSendBufferSize()), MAXSENDBUF);
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
	file->seek(offset);
	
	ssize_t chunk = MIN(uploadBufferSize, total - offset);
	if (chunk)
	{
		ssize_t in = file->read(uploadBuffer, chunk);
		ssize_t out = socket->write(uploadBuffer, in);
		QDBG("Upload::transfer, in=%d, out=%d", (int) in, (int) out);
		rate.add(out);
		offset += out;
	}
}


QuickDC::ZUpload::ZUpload(TransferManager* mgr, PrivateTransferListener* listener, Samurai::IO::Net::Socket* conn, Samurai::IO::File* file_, off_t offset, off_t length)
	: QuickDC::Upload(mgr, listener, conn, file_, offset, length)
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

		ssize_t chunk = MIN(compressBufferSize, total - offset);
		ssize_t in = file->read(compressBuffer, (size_t) chunk);
		if (in <= 0)
		{
			QERR("ZUpload: unable to read from '%s'", file->getName().c_str());
			status = IOFail;
			return;
		}
		consumed = (size_t) in;
	}

	/* A zero length input is what tells the codec to flush. */
	const bool flushing = (consumed == 0);

	size_t produced = pendingCapacity;
	if (!compressor->exec(compressBuffer, consumed, pendingBuffer, produced))
	{
		QERR("ZUpload: deflate failed");
		status = IOFail;
		return;
	}

	/* Only what the compressor actually took advances the file position. */
	offset += consumed;

	if (flushing && !produced)
		finished = true;

	pendingSize = produced;
	pendingOffset = 0;

	if (offset > (total - length))
		ratio = (double) (compressedBytes + produced) / (double) (offset - (total - length));

	QDBG("ZUpload::transfer, consumed=%d, produced=%d, at %d/%d",
		(int) consumed, (int) produced, (int) offset, (int) total);

	flushPending();
}



QuickDC::Download::Download(TransferManager* mgr, PrivateTransferListener* listener, Samurai::IO::Net::Socket* conn, Samurai::IO::File* file_, off_t offset, off_t length)
	: QuickDC::Transfer(mgr, listener, conn, file_, QuickDC::Transfer::Download, offset, length)
	, downloadBuffer(0), downloadBufferSize(0)
{
	QDBG("QuickDC::Download::Download()");

	/*
	 * Write only, and deliberately without Truncate: we are filling in a
	 * partial file that may already hold chunks fetched from other sources.
	 * File::open() adds O_CREAT for a write-only open, so a brand new
	 * download does not need special casing.
	 */
	if (!file->open(Samurai::IO::File::Write))
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

	downloadBufferSize = MIN(((size_t) socket->getReceiveBufferSize()), (size_t) MAXSENDBUF);
	if (!downloadBufferSize) downloadBufferSize = 8192;
	downloadBuffer = new char[downloadBufferSize];
}

QuickDC::Download::~Download() {
	QDBG("QuickDC::Download::~Download()");
	file->close();
	delete[] downloadBuffer;
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

	ssize_t in = socket->read(downloadBuffer, want);
	if (in <= 0)
	{
		/*
		 * 0 is the peer closing, negative is an error. Either way the rest of
		 * this chunk is never arriving, so fail out instead of being polled
		 * about a socket that will not produce it.
		 */
		if (in < 0)
		{
			QERR("Download: unable to read from the peer");
		}
		else
		{
			QDBG("Download: the peer closed after %d of %d bytes", (int) offset, (int) total);
		}

		status = IOFail;
		return;
	}

	ssize_t out = file->write(downloadBuffer, (size_t) in);
	if (out != in)
	{
		QERR("Download: wrote %d of %d bytes to '%s'",
			(int) out, (int) in, file->getName().c_str());
		status = IOFail;
		return;
	}

	rate.add(out);
	offset += (uint64_t) out;
}










