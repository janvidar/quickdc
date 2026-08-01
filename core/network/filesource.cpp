/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/filesource.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
	: hub(0), cid(0), filename(0), retryCount(0), availability(0), throughput(0.0)
{
	if (!url_) return;

	const char* sep = strstr(url_, "://");
	if (!sep || sep == url_) return;    /* no protocol */

	const char* slash = strchr(sep + 3, '/');
	if (!slash) return;                 /* no client ID, unusable */

	const char* cid_start = slash + 1;
	const char* cid_end = strchr(cid_start, '/');

	if (cid_end)
	{
		if (cid_end == cid_start) return;   /* empty client ID */
		cid = strndup(cid_start, (size_t) (cid_end - cid_start));
		filename = strdup(cid_end + 1);
	}
	else
	{
		if (!*cid_start) return;            /* empty client ID */
		cid = strdup(cid_start);
		filename = 0;                       /* this is the file list */
	}

	hub = strndup(url_, (size_t) (slash - url_));
}

QuickDC::FileSource::FileSource(const char* hub_, const char* cid_, const char* filename_)
	: hub(0), cid(0), filename(0), retryCount(0), availability(0), throughput(0.0)
{
	hub = (hub_) ? strdup(hub_) : 0;
	cid = (cid_) ? strdup(cid_) : 0;
	filename = (filename_) ? strdup(filename_) : 0;
}

QuickDC::FileSource::~FileSource() {
	free(hub);
	free(cid);
	free(filename);
	free(availability);
}

char* QuickDC::FileSource::toURL() const
{
	if (!isValid()) return 0;

	/* hub + '/' + cid + '\0', and optionally '/' + filename */
	size_t length = strlen(hub) + strlen(cid) + 2;
	if (filename) length += strlen(filename) + 1;

	char* out = (char*) malloc(length);
	if (!out) return 0;

	if (filename)
		snprintf(out, length, "%s/%s/%s", hub, cid, filename);
	else
		snprintf(out, length, "%s/%s", hub, cid);

	return out;
}

void QuickDC::FileSource::touch()
{
	lastSeen.reset();
	retryCount = 0;
}

uint64_t QuickDC::FileSource::getThroughput() const
{
	return (throughput > 0.0) ? (uint64_t) throughput : 0;
}

void QuickDC::FileSource::onChunkDone(uint64_t bytes, double seconds)
{
	if (!bytes || seconds <= 0.0) return;

	const double sample = (double) bytes / seconds;

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
	free(availability);
	availability = (indexes && *indexes) ? strdup(indexes) : 0;
}

bool QuickDC::FileSource::getAvailability(uint64_t blockSize, uint64_t fileSize, ChunkMap& out) const
{
	if (!availability || !blockSize || !fileSize) return false;

	ChunkMap decoded(fileSize);
	if (!decoded.fromIndexString(availability, blockSize)) return false;

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
	return (!strcmp(hub, hub_) && !strcmp(cid, cid_));
}

void QuickDC::FileSource::setRetryCount(size_t count)
{
	retryCount = count;
}

void QuickDC::FileSource::setLastSeen(const Samurai::TimeStamp& seen)
{
	lastSeen = seen;
}
