/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"

#include "hash/hashcache.h"
#include "hash/hashmanager.h"
#include "hash/hashstorage.h"

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>

#define HEADER_SIZE_INFO     static_cast<uint8_t>(sizeof(size_t))
#define HEADER_EOL           0x0a
#define HEADER_EOF           0x1b
#define HEADER_VERSION       0x01


QuickDC::Hash::CacheStorage::CacheStorage(const Samurai::IO::File& file_) : readable(false), writable(false), opened(false), file(file_), buffer(sizeof(CacheEntryHeader)*4), position(0)
{

}


QuickDC::Hash::CacheStorage::~CacheStorage()
{
	reset();
}

bool QuickDC::Hash::CacheStorage::open(enum Access access)
{
	if (access == ReadAccess)  return openForReading();
	if (access == WriteAccess) return openForWriting();
	opened = false;
	return false;
}

void QuickDC::Hash::CacheStorage::reset()
{
	if (opened)
	{
		file.close();
		position = 0;
		opened = false;
	}
	readable = false;
	writable = false;
}


bool QuickDC::Hash::CacheStorage::openForReading()
{
	if (!file.exists())
		return false;
		
	if (!file.open(Samurai::IO::File::Mode::Read))
		return false;
	
	position = 0;
	
	CacheMasterRecord master;
	if (!readSection(STORAGE_MAGIC_MASTER, &master, sizeof(master)))
	{
		reset();
		return false;
	}

	if (!(master.size_info == HEADER_SIZE_INFO && master.version == HEADER_VERSION))
	{
		QERR("Invalid header of cache file");
		reset();
		return false;
	}
	
	readable = true;
	opened = true;
	return true;
}


bool QuickDC::Hash::CacheStorage::read(QuickDC::Hash::CacheEntry*& ptr)
{
	QDBG("QuickDC::Hash::CacheStorage::read()");
	ptr = 0;
	if (!opened || !readable) return false;

	size_t size = 0;
	CacheEntryHeader file_header;
	
	size = readSection(STORAGE_MAGIC_RECORD, &file_header, sizeof(file_header));
	if (!size)
		return false;

	/* Whatever the file says, the name is used as a C string from here on. */
	file_header.name[sizeof(file_header.name) - 1] = '\0';

	buffer.clear();
	off_t storedPosition = (position + sizeof(CacheRecordHeader));
	size = readSection(STORAGE_MAGIC_DATA, buffer);
	if (size)
	{
		 ptr = CacheEntry::create(file_header, buffer);
		 if (ptr)
		 {
			 ptr->setCacheFileOffset(storedPosition);
		 }
	}
	return true;
}


size_t QuickDC::Hash::CacheStorage::readSectionHeader(uint32_t magic)
{
	CacheRecordHeader header;
	ssize_t got = file.read(reinterpret_cast<char*>(&header), sizeof(header));
	if (got < static_cast<ssize_t>(sizeof(header)))
	{
		QERR("Unexpected end of record header");
		return 0;
	}
	position += got;
	
	if (header.magic == STORAGE_MAGIC_EOF && magic != header.magic)
	{
		QDBG("End of file reached");
		return 0;
	}
	
	if (header.magic != magic)
	{
		QDBG("Unexpected header found");
		return 0;
	}

	/* The size comes out of the file, and the read below buffers all of it
	   before finding out the file is nowhere near that long. */
	const off_t remaining = file.size() - position;
	if (remaining < 0 || static_cast<uint64_t>(header.size) > static_cast<uint64_t>(remaining))
	{
		QERR("Record claims %u bytes, but %lld remain", header.size, static_cast<long long>(remaining));
		return 0;
	}

	return header.size;
}

size_t QuickDC::Hash::CacheStorage::readSection(uint32_t magic, Samurai::IO::Buffer& buffer)
{
	size_t size = readSectionHeader(magic);
	if (!size) return 0;
	
	ssize_t got = file.read(&buffer, size);
	if (got < static_cast<ssize_t>(size))
	{
		QERR("Unexpected end of file");
		return 0;
	}
	position += got;
	return size;
}

size_t QuickDC::Hash::CacheStorage::readSection(uint32_t magic, void* ptr, size_t sz)
{
	size_t size = readSectionHeader(magic);
	if (!size) return 0;
	
	if (size != sz)
	{
		QERR("readSection: unexpected record size. Expected %zu, but have %zu!", sz, size);
		return 0;
	}
	
	ssize_t got = file.read(static_cast<char*>(ptr), size);
	if (got < static_cast<ssize_t>(size))
	{
		QERR("Unexpected end of file");
		return 0;
	}
	position += got;
	return size;
}

bool QuickDC::Hash::CacheStorage::write(QuickDC::Hash::CacheEntry* entry)
{
	QDBG("QuickDC::Hash::CacheStorage::write(): entry=%p", entry);
	if (!opened || !writable) return false;

	buffer.clear();
	entry->write(&buffer);
	entry->setCacheFileOffset(position + (sizeof(CacheRecordHeader) << 1) + sizeof(CacheEntryHeader));

	size_t length = buffer.size();
	file.write(&buffer, length, true);
	position += length;
	return true;
}


bool QuickDC::Hash::CacheStorage::openForWriting()
{
	if (!file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate))
		return false;
	
	QDBG("QuickDC::Hash::CacheStorage::openForWriting()");
	
	buffer.clear();
	Samurai::TimeStamp timestamp;
	
	CacheRecordHeader header;
	CacheMasterRecord record;
	header.magic        = STORAGE_MAGIC_MASTER;
	header.size         = sizeof(CacheMasterRecord);
	record.size_info    = HEADER_SIZE_INFO;
	record.version      = HEADER_VERSION;
	
	buffer.append(reinterpret_cast<const char*>(&header), sizeof(header));
	buffer.append(reinterpret_cast<const char*>(&record), sizeof(record));

	/* write() consumes the buffer, so the length has to be taken first. */
	size_t length = buffer.size();
	file.write(&buffer, length);
	position = length;
	
	/* close() writes the end of file record only for a storage that says it is
	   writable, and nothing ever said so - every cache we wrote ended without
	   one, so every load ran off the end of the file instead. */
	writable = true;
	opened = true;
	return true;
}

void QuickDC::Hash::CacheStorage::close()
{
	if (opened)
	{
		if (writable)
		{
			QDBG("QuickDC::Hash::CacheStorage::close()");
			CacheRecordHeader header;
			header.magic = STORAGE_MAGIC_EOF;
			header.size  = 0;
			file.write(reinterpret_cast<const char*>(&header), sizeof(header));
		}
		
		reset();
	}
}
