/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HASH_CACHE_STORAGE_H
#define HAVE_QUICKDC_HASH_CACHE_STORAGE_H

#include "quickdc.h"
#include <string>
#include <vector>
#include <map>

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <samurai/crypto/digest/hash.h>

#define STORAGE_MAGIC_MASTER          0x514b4443 /* QKDC */
#define STORAGE_MAGIC_RECORD          0x46494c30 /* FIL0 */
#define STORAGE_MAGIC_DATA            0x44415430 /* DAT0 */
#define STORAGE_MAGIC_EOF             0x454f4630 /* EOF0 */

/**
 * All disk records start with this header.
 */
typedef struct CacheRecordHeader
{
	uint32_t magic;
	uint32_t size;
} CacheRecordHeader;

/**
 * The global file is prefixed with the following header.
 */
typedef struct CacheMasterRecord
{
	size_t size_info; /* sizeof(size_t) 4 on 32-bit machines, 8 on 64-bit machines */
	size_t version;   /* 0 */
} CacheMasterRecord;

typedef struct CacheEntryHeader
{
	char              name[PATH_MAX]; /* full path of file */
	off_t             size;           /* size of file */
	time_t            mtime;          /* modification time of file */
	size_t            algorithm;      /* The hash algorithm used (0=tth) */
	size_t            leaves;         /* The number of leaf hashes kept (at least 1)  */
	size_t            depth;          /* The depth of the merkle tree (if any, or 0 if no merkle tree) */
} CacheEntryHeader;


namespace QuickDC {
namespace Hash {

class CacheEntry;

/**
 * This contains the storage for the hash cache.
 * Basically all leaf nodes are stored in this file.
 */

class CacheStorage
{
	public:
		enum Access { ReadAccess, WriteAccess };

	public:
		CacheStorage(const Samurai::IO::File& file);
		~CacheStorage();
	
		/**
		 * Open the storage for writing or reading depending
		 * on if we are in WriteAccess or ReadAccess mode (when constructed).
		 */
		bool open(enum Access);
		void close();
		
		/**
		 * Read a record from the storage (only works in ReadAccess mode).
		 * If a valid record is found ptr is non-null, otherwise null.
		 *
		 * @return true if more data can be read, or false if EOF is reached.
		 */
		bool read(QuickDC::Hash::CacheEntry*& ptr);
		
		/**
		 * Write a record to the storage (append it). Only works in WriteAccess mode.
		 */
		bool write(QuickDC::Hash::CacheEntry*);
		

	private:
		bool openForReading();
		bool openForWriting();
		void reset();
		
		/**
		 * Reads a section header, and returns the size of the next section.
		 * NOTE: EOF has a size of zero.
		 */
		size_t readSectionHeader(uint32_t magic);
		size_t readSection(uint32_t magic, Samurai::IO::Buffer& buffer);
		size_t readSection(uint32_t magic, void* ptr, size_t sz);
		
	private:
		bool readable;
		bool writable;
		bool opened;
		Samurai::IO::File file;
		Samurai::IO::Buffer buffer;
		off_t position;             // current read/write position in file.
};

}
}

#endif // HAVE_QUICKDC_HASH_CACHE_STORAGE_H
