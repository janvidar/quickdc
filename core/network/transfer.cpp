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
	: manager(mgr), listener(listener_), socket(sock), file(file_), direction(dir_), ratio(1.0), offset(offset_), length(length_)
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
	
	if (!file->isOpen())
	{
		QERR("QuickDC::Transfer::exec(): file is not open!");
		return false;
	}
	
	transfer();
	
	if (offset >= total)
	{
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
	: QuickDC::Upload(mgr, listener, conn, file_, offset, length), compressor(0), compressBuffer(0), compressBufferSize(0)
{
	compressor = new Samurai::IO::GzipCompressor();
	compressBufferSize = (uploadBufferSize << 1);
	compressBuffer = new char[compressBufferSize];
}

QuickDC::ZUpload::~ZUpload()
{
	delete compressor;
	delete[] compressBuffer;
}


void QuickDC::ZUpload::close()
{
	Upload::close();
	// FIXME: anything specialized
}

void QuickDC::ZUpload::transfer()
{
	QDBG("ZUpload: Seek to %d\n", (int) offset);
	file->seek(offset);
	
	ssize_t in = 0;
	ssize_t chunk = MIN(compressBufferSize, total - offset);
	if (chunk)
	{
		in = file->read(compressBuffer, chunk);
		if (in < 0)
			in = 0;
	}
	
	size_t compressOut = uploadBufferSize;
	size_t after = (size_t) in;
	compressor->exec(compressBuffer, after, uploadBuffer, compressOut);
	QDBG("ZUpload::transfer, in=(%d -> %d), compressOut=%d, bufsizes=%d/%d", (int) in, (int) after, (int) compressOut, (int) uploadBufferSize, (int) compressBufferSize);
	
	ssize_t out = socket->write(uploadBuffer, compressOut);
	rate.add(out);
	offset += out;
}



QuickDC::Download::Download(TransferManager* mgr, PrivateTransferListener* listener, Samurai::IO::Net::Socket* conn, Samurai::IO::File* file_, off_t offset, off_t length) : QuickDC::Transfer(mgr, listener, conn, file_, QuickDC::Transfer::Download, offset, length)
{
	QDBG("QuickDC::Download::Download()");
}

QuickDC::Download::~Download() {
	QDBG("QuickDC::Download::~Download()");
}

void QuickDC::Download::close()
{
	file->close();
	manager->onTransferStopped(this);
	QDBG("Download finished.");
}

void QuickDC::Download::transfer()
{
	/*
	uint len = stats.suggestNextChunk();
	off_t offset = file->getCurrentPosition();
	if (offset + len > file->size()) len = file->size() - offset;
	
	if (len > MAXSENDBUF) len = MAXSENDBUF;
	
	char* buffer = new char[len];
	uint in = file->read(buffer, len);
	uint out = socket->write(buffer, in);
	
	file->seek(offset+out);
	delete[] buffer;
	*/
}










