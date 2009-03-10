/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_TRANSFER_H
#define HAVE_QUICKDC_TRANSFER_H

#include "quickdc.h"
#include <samurai/util/bwestimation.h>
#include <samurai/io/compression.h>
#include <time.h>
#include <string>

namespace Samurai {
	namespace IO {
		class File;
		namespace Net {
			class Socket;
		}
	}
}

namespace QuickDC {

class TransferManager;
class ClientSession;

class PrivateTransferListener {
	public:
	
		virtual ~PrivateTransferListener() { }

		/**
		 * Transfer stopped, you need to check the status in the 
		 * transfer object.
		 */
		virtual void onTransferStopped() = 0;
};

/**
 * Base of all transfers.
 */
class Transfer {
	public:
		enum Direction { Download, Upload };
		enum TransferStatus { OK, IOFail, RollbackFail };
		enum TransferError { OutOfDisk };
		
		/**
		 *
		 */
		Transfer(TransferManager*, PrivateTransferListener*, Samurai::IO::Net::Socket*, Samurai::IO::File*, Direction, off_t offset, off_t length);
		virtual ~Transfer();
		
		/**
		 * Transfer one chunk of data, for upload we will send
		 * the amount acceptable by the low level send buffer,
		 * and for download we will receive whatever is available.
		 */
		virtual bool exec();
		
		inline bool isUpload() const { return direction == Upload; }
		inline bool isDownload() const { return direction == Download; }
		
		/**
		 * The ratio of data going through the networking.
		 * Unless compression is involved, this is always 1.0
		 */
		inline double getRatio() const { return ratio; }
		
		/**
		 * Returns the current transfer rate (bytes per second).
		 * Note: this number is estimated based on the last few seconds
		 * of traffic, and this operation is designed to be fairly cheap.
		 */
		size_t getBps() { return rate.getBps(); }
		
		/**
		 * Return an instance of the client session
		 */
		PrivateTransferListener* getPrivateTransferListener() { return listener; }
		
		
		Samurai::IO::File* getFile() { return file; }
		Samurai::IO::Net::Socket* getSocket() { return socket; }
	
	protected:
		virtual void transfer() = 0;
		virtual void close()    = 0;
		
	protected:
		TransferManager* manager;
		PrivateTransferListener* listener;
		Samurai::IO::Net::Socket* socket;
		Samurai::IO::File* file;
		enum Direction direction;
		Samurai::Util::RateEstimator rate;
		double ratio;
		
		uint64_t offset;
		uint64_t length;
		uint64_t total;
		
};

class Upload : public Transfer
{
	public:
		Upload(TransferManager*, PrivateTransferListener*, Samurai::IO::Net::Socket*, Samurai::IO::File*, off_t offset, off_t length);
		virtual ~Upload();

	protected:
		virtual void transfer();
		virtual void close();
		
	protected:
		char* uploadBuffer;
		size_t uploadBufferSize;
};

class ZUpload : public Upload
{
	public:
		ZUpload(TransferManager*, PrivateTransferListener*, Samurai::IO::Net::Socket*, Samurai::IO::File*, off_t offset, off_t length);
		virtual ~ZUpload();
		
	protected:
		virtual void transfer();
		virtual void close();
		
	protected:
		Samurai::IO::GzipCompressor* compressor;
		char* compressBuffer;
		size_t compressBufferSize;
};

class Download : public Transfer {
	public:
		Download(TransferManager*, PrivateTransferListener*, Samurai::IO::Net::Socket*, Samurai::IO::File*, off_t offset, off_t length);
		virtual ~Download();
		
	protected:
		virtual void transfer();
		virtual void close();
};

}

#endif // HAVE_QUICKDC_TRANSFER_H
