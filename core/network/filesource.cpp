/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/filesource.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string_view>
#include <vector>

/*
 * Split "{proto}://{host}:{port}/{cid}[/{filename}]" into its three parts.
 *
 * Everything up to the slash that follows the authority is the hub URL, the
 * next path segment is the client ID, and whatever remains is the filename.
 * A URL with no slash after the client ID has no filename at all, which is
 * how a file list is spelled; a trailing slash gives an empty filename,
 * which is the root directory.
 */
QuickDC::FileSource::FileSource(const char* url_)
	: retryCount(0), throughput(0.0)
{
	if (!url_) return;

	const std::string_view url(url_);

	const size_t proto = url.find("://");
	if (proto == std::string_view::npos || !proto) return;   /* no protocol */

	const size_t slash = url.find('/', proto + 3);
	if (slash == std::string_view::npos) return;             /* no client ID, unusable */

	const std::string_view rest = url.substr(slash + 1);
	const size_t next = rest.find('/');

	if (next != std::string_view::npos)
	{
		if (!next) return;                  /* empty client ID */
		cid = std::string(rest.substr(0, next));
		filename = std::string(rest.substr(next + 1));
	}
	else
	{
		if (rest.empty()) return;           /* empty client ID */
		cid = std::string(rest);
		/* No filename at all: this is the file list. */
	}

	hub = std::string(url.substr(0, slash));
}

QuickDC::FileSource::FileSource(const char* hub_, const char* cid_, const char* filename_)
	: retryCount(0), throughput(0.0)
{
	if (hub_) hub = hub_;
	if (cid_) cid = cid_;
	if (filename_) filename = filename_;
}

QuickDC::FileSource::~FileSource() {
}

char* QuickDC::FileSource::toURL() const
{
	if (!isValid()) return 0;

	std::string url = *hub + '/' + *cid;
	if (filename) url += '/' + *filename;

	/* The caller frees this, so it has to come from the C allocator. */
	return strdup(url.c_str());
}

void QuickDC::FileSource::touch()
{
	lastSeen.reset();
	retryCount = 0;
}

uint64_t QuickDC::FileSource::getThroughput() const
{
	return (throughput > 0.0) ? static_cast<uint64_t>(throughput) : 0;
}

void QuickDC::FileSource::onChunkDone(uint64_t bytes, double seconds)
{
	if (!bytes || seconds <= 0.0) return;

	const double sample = static_cast<double>(bytes) / seconds;

	/*
	 * Smoothed rather than taken as read. One chunk is a small sample and a
	 * peer's throughput wanders, so a single slow chunk must not shrink the next
	 * request to nothing and a single fast one must not ask for the world. The
	 * weight is light enough to follow a source that really has changed speed
	 * within a few chunks.
	 */
	throughput = (throughput > 0.0) ? (throughput * 0.7 + sample * 0.3) : sample;
}

void QuickDC::FileSource::onFailure()
{
	retryCount++;
}

void QuickDC::FileSource::onAttempt()
{
	lastAttempt.reset();
}

void QuickDC::FileSource::setAvailability(const char* indexes)
{
	availability = indexes ? indexes : "";
}

bool QuickDC::FileSource::getAvailability(uint64_t blockSize, uint64_t fileSize, ChunkMap& out) const
{
	if (availability.empty() || !blockSize || !fileSize) return false;

	ChunkMap decoded(fileSize);
	if (!decoded.fromIndexString(availability.c_str(), blockSize)) return false;

	out = decoded;
	return true;
}

bool QuickDC::FileSource::isReadyToRetry() const
{
	if (!retryCount) return true;

	time_t now = Samurai::TimeStamp().getInternalData();
	time_t last = lastAttempt.getInternalData();

	return (now < last || (now - last) >= QUICKDC_SOURCE_RETRY_INTERVAL);
}

bool QuickDC::FileSource::isExpired() const
{
	if (retryCount > QUICKDC_SOURCE_MAX_RETRIES)
		return true;

	time_t now = Samurai::TimeStamp().getInternalData();
	time_t seen = lastSeen.getInternalData();

	return (now > seen && (now - seen) > QUICKDC_SOURCE_MAX_AGE);
}

bool QuickDC::FileSource::matches(const char* hub_, const char* cid_) const
{
	if (!isValid() || !hub_ || !cid_) return false;
	return (*hub == hub_ && *cid == cid_);
}

void QuickDC::FileSource::setRetryCount(size_t count)
{
	retryCount = count;
}

void QuickDC::FileSource::setLastSeen(const Samurai::TimeStamp& seen)
{
	lastSeen = seen;
}
