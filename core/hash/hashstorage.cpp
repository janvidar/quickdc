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

#define HEADER_SIZE_INFO     (uint8_t) sizeof(size_t)
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
}


bool QuickDC::Hash::CacheStorage::openForReading()
{
	if (!file.exists())
		return false;
		
	if (!file.open(Samurai::IO::File::Read))
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
	
	opened = true;
	return true;
}


bool QuickDC::Hash::CacheStorage::read(QuickDC::Hash::CacheEntry*& ptr)
{
	QDBG("QuickDC::Hash::CacheStorage::read()");
	ptr = 0;
	size_t size = 0;
	CacheEntryHeader file_header;
	
	size = readSection(STORAGE_MAGIC_RECORD, &file_header, sizeof(file_header));
	if (!size)
		return false;
	
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
	ssize_t got = file.read((char*) &header, sizeof(header));
	if (got < (ssize_t) sizeof(header))
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

	return header.size;
}

size_t QuickDC::Hash::CacheStorage::readSection(uint32_t magic, Samurai::IO::Buffer& buffer)
{
	size_t size = readSectionHeader(magic);
	if (!size) return 0;
	
	ssize_t got = file.read(&buffer, size);
	if (got < (ssize_t) size)
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
	
	ssize_t got = file.read((char*) ptr, size);
	if (got < (ssize_t) size)
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
	buffer.clear();
	entry->write(&buffer);
	entry->setCacheFileOffset(position + (sizeof(CacheRecordHeader) << 1) + sizeof(CacheEntryHeader));
	file.write(&buffer, buffer.size(), true);
	position += buffer.size();
	return true;
}


bool QuickDC::Hash::CacheStorage::openForWriting()
{
	if (!file.open(Samurai::IO::File::Write | Samurai::IO::File::Truncate))
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
	
	buffer.append((const char*) &header, sizeof(header));
	buffer.append((const char*) &record, sizeof(record));
	file.write(&buffer, buffer.size());
	position = buffer.size();
	
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
			file.write((const char*) &header, sizeof(header));
		}
		
		reset();
	}
}
