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
		 * @return true once every byte we asked for has been transferred.
		 * This is what tells a download whether the chunk can be committed
		 * to the queue, or has to be handed back for someone else to fetch.
		 */
		inline bool isFinished() const { return offset >= total; }

		inline enum TransferStatus getStatus() const { return status; }

		/** How far we have got, in absolute file position. */
		inline uint64_t getOffset() const { return offset; }
		inline uint64_t getTotal() const { return total; }

		/** How many bytes this transfer covers in total. */
		inline uint64_t getLength() const { return length; }

		/** How many of them have moved so far. */
		inline uint64_t getTransferred() const { return offset - (total - length); }

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

		/**
		 * Whether the transfer is done and may be closed.
		 *
		 * Having consumed every input byte is enough for a plain transfer, but
		 * a compressed one still has to flush the codec and push the last
		 * compressed bytes out, so ZUpload extends this.
		 */
		virtual bool isTransferComplete() const { return offset >= total; }

	protected:
		TransferManager* manager;
		PrivateTransferListener* listener;
		Samurai::IO::Net::Socket* socket;
		Samurai::IO::File* file;
		enum Direction direction;
		Samurai::Util::RateEstimator rate;
		double ratio;

		/** Anything other than OK stops the transfer at the next exec(). */
		enum TransferStatus status;

		/** close() has been called; exec() must not run again. */
		bool closed;

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

/**
 * An upload that deflates the file on the way out (the ADC "ZLIG" extension).
 *
 * Compression makes the bookkeeping different from a plain Upload in three
 * ways, and all three have to be respected or the peer receives a stream it
 * cannot inflate:
 *
 *  - 'offset' is a position in the uncompressed file, so it advances by what
 *    the compressor consumed, never by what went out on the wire.
 *  - compressed output cannot be regenerated, so a short socket write has to
 *    be buffered and retried rather than dropped.
 *  - deflate has to be flushed at the end, which produces bytes after the
 *    last input byte has already been consumed.
 */
class ZUpload : public Upload
{
	public:
		ZUpload(TransferManager*, PrivateTransferListener*, Samurai::IO::Net::Socket*, Samurai::IO::File*, off_t offset, off_t length);
		virtual ~ZUpload();

	protected:
		virtual void transfer();
		virtual void close();

		/** Also requires the codec to be flushed and the buffer drained. */
		virtual bool isTransferComplete() const;

	private:
		/**
		 * Push out whatever compressed data is still buffered.
		 * @return true once the buffer is empty.
		 */
		bool flushPending();

	protected:
		Samurai::IO::GzipCompressor* compressor;

		/** Holds uncompressed bytes read from the file. */
		char* compressBuffer;
		size_t compressBufferSize;

		/** Holds compressed bytes on their way to the socket. */
		char* pendingBuffer;
		size_t pendingCapacity;
		size_t pendingSize;     /* valid bytes in pendingBuffer */
		size_t pendingOffset;   /* how much of pendingSize has been written */

		/** deflate() has been flushed and has no more output to give. */
		bool finished;

		/** Total bytes actually written, for getRatio(). */
		uint64_t compressedBytes;
};

class Download : public Transfer {
	public:
		Download(TransferManager*, PrivateTransferListener*, Samurai::IO::Net::Socket*, Samurai::IO::File*, off_t offset, off_t length);
		virtual ~Download();

		/**
		 * Take over bytes already read from the socket by the command parser,
		 * to be written before anything more is read.
		 */
		void pushPending(const char* data, size_t length);

	protected:
		virtual void transfer();
		virtual void close();

	protected:
		char* downloadBuffer;
		size_t downloadBufferSize;

		char* pending;
		size_t pendingSize;
		size_t pendingOffset;
};

}

#endif // HAVE_QUICKDC_TRANSFER_H
