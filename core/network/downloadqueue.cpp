/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/downloadqueue.h"
#include "network/filesource.h"
#include "network/hubmanager.h"
#include "api/core.h"
#include "api/hub.h"
#include "config/preferences.h"
#include "hash/hashmanager.h"
#include "hash/hashjob.h"
#include "xml/reader.h"
#include "share/dcfilelist.h"

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <samurai/io/compression.h>

#include <stdio.h>

/* Defined in xml/writer.cpp, which does not declare it in a header. */
namespace XML
{
	extern void escape(Samurai::IO::Buffer* buffer, const char* str);
}

#define XML_NEWLINE "\r\n"
#define XML_INDENT "\x9"

/* ------------------------------------------------------------------------- */
/* QueueFile                                                                 */
/* ------------------------------------------------------------------------- */

QuickDC::QueueFile::QueueFile(const char* filename_, const char* tth_, uint64_t size_)
	: filename(0)
	, tempname(0)
	, tth(0)
	, size(size_)
	, priority(PrioNormal)
	, chunks(size_)
{
	filename = strdup(filename_ ? filename_ : "");

	size_t length = strlen(filename) + strlen(QUICKDC_PART_SUFFIX) + 1;
	tempname = (char*) malloc(length);
	snprintf(tempname, length, "%s%s", filename, QUICKDC_PART_SUFFIX);

	tth = (tth_) ? strdup(tth_) : 0;
}

QuickDC::QueueFile::~QueueFile()
{
	while (sources.size())
	{
		FileSource* source = sources.back();
		sources.pop_back();
		delete source;
	}
	active.clear();

	free(filename);
	free(tempname);
	free(tth);
}

const char* QuickDC::QueueFile::convertPriority(enum Priority prio)
{
	switch (prio)
	{
		case PrioPaused: return "paused";
		case PrioLow:    return "low";
		case PrioNormal: return "normal";
		case PrioHigh:   return "high";
	}
	return "normal";
}

enum QuickDC::QueueFile::Priority QuickDC::QueueFile::convertPriority(const char* str)
{
	if (!str) return PrioNormal;
	if (!strcasecmp(str, "paused")) return PrioPaused;
	if (!strcasecmp(str, "low"))    return PrioLow;
	if (!strcasecmp(str, "high"))   return PrioHigh;
	return PrioNormal;
}

void QuickDC::QueueFile::setSize(uint64_t size_)
{
	size = size_;
	chunks.setSize(size_);
}

void QuickDC::QueueFile::addSource(FileSource* source)
{
	if (!source) return;

	if (!source->isValid())
	{
		QDBG("Ignoring an unusable file source");
		delete source;
		return;
	}

	if (findSource(source->getHubUrl(), source->getClientID()))
	{
		/* Already known; keep the one we have, it carries the history. */
		delete source;
		return;
	}

	sources.push_back(source);
}

void QuickDC::QueueFile::removeSource(FileSource* source)
{
	if (!source) return;

	for (std::vector<FileSource*>::iterator it = sources.begin(); it != sources.end(); it++)
	{
		if (*it == source)
		{
			sources.erase(it);
			break;
		}
	}

	active.erase(source);
	delete source;
}

void QuickDC::QueueFile::expireSources()
{
	std::vector<FileSource*> dead;

	for (size_t n = 0; n < sources.size(); n++)
	{
		/* Never drop a source we are talking to right now. */
		if (isSourceActive(sources[n])) continue;
		if (sources[n]->isExpired()) dead.push_back(sources[n]);
	}

	for (size_t n = 0; n < dead.size(); n++)
	{
		QDBG("Forgetting a stale source for '%s'", filename);
		removeSource(dead[n]);
	}
}

size_t QuickDC::QueueFile::countSources() const
{
	return sources.size();
}

size_t QuickDC::QueueFile::countActiveSources() const
{
	return active.size();
}

QuickDC::FileSource* QuickDC::QueueFile::getSource(size_t n) const
{
	if (n >= sources.size()) return 0;
	return sources[n];
}

QuickDC::FileSource* QuickDC::QueueFile::findSource(const char* hub, const char* cid) const
{
	for (size_t n = 0; n < sources.size(); n++)
	{
		if (sources[n]->matches(hub, cid)) return sources[n];
	}
	return 0;
}

QuickDC::FileSource* QuickDC::QueueFile::getIdleSource() const
{
	for (size_t n = 0; n < sources.size(); n++)
	{
		if (isSourceActive(sources[n])) continue;
		if (sources[n]->isExpired()) continue;
		return sources[n];
	}
	return 0;
}

void QuickDC::QueueFile::setSourceActive(FileSource* source, bool active_)
{
	if (!source) return;

	if (active_)
		active.insert(source);
	else
		active.erase(source);
}

bool QuickDC::QueueFile::isSourceActive(FileSource* source) const
{
	return (active.count(source) != 0);
}

bool QuickDC::QueueFile::isComplete() const
{
	return chunks.isComplete();
}

uint64_t QuickDC::QueueFile::getDoneBytes() const
{
	return chunks.getDoneBytes();
}

bool QuickDC::QueueFile::isPersistent() const
{
	if (!tth || strlen(tth) != QUICKDC_TTH_LENGTH) return false;
	if (!size) return false;
	return !isComplete();
}

/* ------------------------------------------------------------------------- */
/* FilelistRequest                                                           */
/* ------------------------------------------------------------------------- */

QuickDC::FilelistRequest::FilelistRequest(const char* hub_, const char* cid_)
	: hub(0)
	, cid(0)
	, filename(0)
	, tempname(0)
	, retryCount(0)
	, active(false)
{
	hub = strdup(hub_ ? hub_ : "");
	cid = strdup(cid_ ? cid_ : "");

	/* directory + '/' + cid + ".xml" + '\0' */
	size_t length = strlen(QUICKDC_LIST_DIR) + strlen(cid) + 6;
	filename = (char*) malloc(length);
	snprintf(filename, length, "%s/%s.xml", QUICKDC_LIST_DIR, cid);

	size_t templength = strlen(filename) + strlen(QUICKDC_PART_SUFFIX) + 1;
	tempname = (char*) malloc(templength);
	snprintf(tempname, templength, "%s%s", filename, QUICKDC_PART_SUFFIX);
}

QuickDC::FilelistRequest::~FilelistRequest()
{
	free(hub);
	free(cid);
	free(filename);
	free(tempname);
}

bool QuickDC::FilelistRequest::matches(const char* hub_, const char* cid_) const
{
	if (!hub_ || !cid_) return false;
	return (!strcmp(hub, hub_) && !strcmp(cid, cid_));
}

void QuickDC::FilelistRequest::onAttempt()
{
	lastAttempt.reset();
}

void QuickDC::FilelistRequest::onFailure()
{
	retryCount++;
}

bool QuickDC::FilelistRequest::isReadyToRetry() const
{
	/* The first attempt is made straight away. */
	if (!retryCount) return true;

	time_t now = Samurai::TimeStamp().getInternalData();
	time_t last = lastAttempt.getInternalData();

	return (now < last || (now - last) >= QUICKDC_FILELIST_RETRY_INTERVAL);
}

/* ------------------------------------------------------------------------- */
/* PendingRequest                                                            */
/* ------------------------------------------------------------------------- */

QuickDC::PendingRequest::PendingRequest(const char* token_, QueueFile* file_, FileSource* source_)
	: token(strdup(token_ ? token_ : ""))
	, kind(KindFile)
	, file(file_)
	, source(source_)
	, filelist(0)
	, reserved(false)
	, offset(0)
	, length(0)
{

}

QuickDC::PendingRequest::PendingRequest(const char* token_, FilelistRequest* filelist_)
	: token(strdup(token_ ? token_ : ""))
	, kind(KindFilelist)
	, file(0)
	, source(0)
	, filelist(filelist_)
	, reserved(false)
	, offset(0)
	, length(0)
{

}

QuickDC::PendingRequest::~PendingRequest()
{
	free(token);
}

void QuickDC::PendingRequest::setReservation(uint64_t offset_, uint64_t length_)
{
	offset = offset_;
	length = length_;
	reserved = true;
}

void QuickDC::PendingRequest::clearReservation()
{
	reserved = false;
	offset = 0;
	length = 0;
}

/* ------------------------------------------------------------------------- */
/* QueueVerifier - bridges the hash manager back into the queue              */
/* ------------------------------------------------------------------------- */

namespace QuickDC {

class QueueVerifier : public QuickDC::Hash::JobListener
{
	public:
		QueueVerifier(QueueManager* queue_) : queue(queue_) { }
		virtual ~QueueVerifier() { }

	public:
		void EventFileStarted(const QuickDC::Hash::Job*) { }
		void EventFileQueued(const QuickDC::Hash::Job*) { }
		void EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t, uint64_t) { }

		void EventFileHashed(const QuickDC::Hash::Job* job)
		{
			if (!job) return;

			/*
			 * Hash::Manager::addJob() calls straight back into here when the
			 * file is already cached, so this must not touch the queue;
			 * onFileVerified() only records the result for process() to apply.
			 *
			 * getHashSum() also hands back a pointer to a static buffer, which
			 * is another reason not to hold on to it.
			 */
			queue->onFileVerified(job->getFile().getName().c_str(), job->getHashSum());
		}

	protected:
		QueueManager* queue;
};

/* ------------------------------------------------------------------------- */
/* QueueXMLReader - reads ~/.quickdc/queue.xml                               */
/* ------------------------------------------------------------------------- */

class QueueXMLReader : public XML::SAX::ContentHandler
{
	public:
		QueueXMLReader(QueueManager* queue_, Samurai::IO::Buffer* buffer)
			: reader(0)
			, queue(queue_)
			, current(0)
			, broken(false)
			, dropped(0)
		{
			reader = new XML::SAX::XMLReader(buffer, this);
		}

		virtual ~QueueXMLReader()
		{
			delete reader;
			delete current;
		}

		bool parse()
		{
			reader->parse();

			if (dropped)
				QERR("Discarded %d unusable entry/entries while loading the download queue",
					(int) dropped);

			return !broken;
		}

	public:
		void startDocument() { }
		void endDocument() { }
		void characters(const char*, size_t, size_t) { }
		void ignorableWhitespace(const char*, size_t, size_t) { }

		void warning(const char* str) { QDBG("queue.xml: %s", str); }
		void error(const char* str) { QERR("queue.xml: %s", str); }
		void fatalError(const char* str) { QERR("queue.xml: %s", str); broken = true; }

		void startElement(const char*, const char* localName, const char*, AttributeList list)
		{
			if (!strcasecmp(localName, "File"))
				startFile(list);
			else if (!strcasecmp(localName, "Source"))
				startSource(list);
			else if (!strcasecmp(localName, "Chunk"))
				startChunk(list);
		}

		void endElement(const char*, const char* localName, const char*)
		{
			if (strcasecmp(localName, "File")) return;

			if (current)
			{
				queue->add(current);
				current = 0;
			}
		}

	protected:
		static const char* getAttribute(AttributeList& list, const char* name)
		{
			for (size_t n = 0; n < list.size(); n++)
			{
				if (!strcasecmp(list[n].first, name)) return list[n].second;
			}
			return 0;
		}

		void startFile(AttributeList& list)
		{
			if (current)
			{
				error("Nested <File>");
				return;
			}

			const char* name = getAttribute(list, "name");
			const char* s_size = getAttribute(list, "size");
			const char* tth = getAttribute(list, "tth");
			const char* s_added = getAttribute(list, "added");
			const char* prio = getAttribute(list, "priority");

			if (!name || !*name || !s_size || !tth)
			{
				error("<File> is missing name, size or tth");
				dropped++;
				return;
			}

			if (strlen(tth) != QUICKDC_TTH_LENGTH)
			{
				error("<File> has a malformed tth");
				dropped++;
				return;
			}

			uint64_t size = quickdc_atoull(s_size);
			if (!size)
			{
				error("<File> has a zero or unreadable size");
				dropped++;
				return;
			}

			current = new QueueFile(name, tth, size);
			current->setPriority(QueueFile::convertPriority(prio));

			if (s_added)
				current->added = Samurai::TimeStamp((time_t) quickdc_atoll(s_added));
		}

		void startSource(AttributeList& list)
		{
			if (!current) return;

			const char* url = getAttribute(list, "url");
			if (!url || !*url)
			{
				error("<Source> is missing its url");
				return;
			}

			FileSource* source = new FileSource(url);
			if (!source->isValid())
			{
				error("<Source> has an unusable url");
				delete source;
				return;
			}

			const char* s_retries = getAttribute(list, "retries");
			const char* s_lastseen = getAttribute(list, "lastseen");

			if (s_retries)
				source->setRetryCount((size_t) quickdc_atoi(s_retries));
			if (s_lastseen)
				source->setLastSeen(Samurai::TimeStamp((time_t) quickdc_atoll(s_lastseen)));

			if (source->isExpired())
			{
				/* Failed too often, or not seen for too long, to be worth keeping. */
				delete source;
				return;
			}

			current->addSource(source);
		}

		void startChunk(AttributeList& list)
		{
			if (!current) return;

			const char* s_start = getAttribute(list, "start");
			const char* s_end = getAttribute(list, "end");

			if (!s_start || !s_end)
			{
				error("<Chunk> is missing start or end");
				return;
			}

			uint64_t start = quickdc_atoull(s_start);
			uint64_t end = quickdc_atoull(s_end);

			if (end <= start || end > current->getSize())
			{
				error("<Chunk> does not fit inside the file");
				return;
			}

			/* addRange() coalesces, so overlapping input cannot inflate how
			   much we believe is already on disk. */
			current->getChunks().addRange(start, end);
		}

	protected:
		XML::SAX::XMLReader* reader;
		QueueManager* queue;
		QueueFile* current;
		bool broken;
		size_t dropped;
};

}

/* ------------------------------------------------------------------------- */
/* QueueManager                                                              */
/* ------------------------------------------------------------------------- */

QuickDC::QueueManager::QueueManager()
	: verifier(0)
	, dirty(false)
	, tokenCounter(0)
{
	QDBG("QueueManager::QueueManager()");

	verifier = new QueueVerifier(this);
	fileIterator = files.end();

	load();
}

QuickDC::QueueManager::~QueueManager()
{
	save();

	while (files.size())
	{
		QueueFile* file = files.back();
		files.pop_back();
		delete file;
	}

	while (filelists.size())
	{
		FilelistRequest* request = filelists.back();
		filelists.pop_back();
		delete request;
	}

	for (std::map<std::string, PendingRequest*>::iterator it = pending.begin(); it != pending.end(); it++)
		delete (*it).second;
	pending.clear();

	delete verifier;
	verifier = 0;
}

void QuickDC::QueueManager::process()
{
	processVerifications();
	expire();
	schedule();

	if (dirty && lastSaved.elapsed() >= QUICKDC_QUEUE_SAVE_INTERVAL)
		save();
}

/* ---- contents ---- */

QuickDC::QueueFile* QuickDC::QueueManager::add(const char* filename, const char* tth, uint64_t size)
{
	QueueFile* existing = getFileByName(filename);
	if (existing)
	{
		/* A search reply may know things the original request did not. */
		if (!existing->getSize() && size)
			existing->setSize(size);

		if (!existing->getTTH() && tth)
		{
			existing->tth = strdup(tth);
			dirty = true;
		}

		return existing;
	}

	QueueFile* file = new QueueFile(filename, tth, size);
	add(file);
	return file;
}

void QuickDC::QueueManager::add(QueueFile* file)
{
	if (!file) return;

	QDBG("Adding file to download queue: %s", file->getLocalFilename());

	files.push_back(file);
	fileIterator = files.end();

	if (file->isPersistent()) dirty = true;
}

void QuickDC::QueueManager::remove(QueueFile* file)
{
	if (!file) return;

	QDBG("Removing file from download queue: %s", file->getLocalFilename());

	clearRequestsFor(file);

	for (std::vector<QueueFile*>::iterator it = files.begin(); it != files.end(); it++)
	{
		if (*it == file)
		{
			files.erase(it);
			break;
		}
	}

	fileIterator = files.end();
	dirty = true;
	delete file;
}

size_t QuickDC::QueueManager::size() const
{
	return files.size();
}

QuickDC::QueueFile* QuickDC::QueueManager::getFileByTTH(const char* tth) const
{
	if (!tth) return 0;

	for (size_t n = 0; n < files.size(); n++)
	{
		const char* have = files[n]->getTTH();
		if (have && !strcmp(have, tth)) return files[n];
	}
	return 0;
}

QuickDC::QueueFile* QuickDC::QueueManager::getFileByName(const char* localFilename) const
{
	if (!localFilename) return 0;

	for (size_t n = 0; n < files.size(); n++)
	{
		if (!strcmp(files[n]->getLocalFilename(), localFilename)) return files[n];
	}
	return 0;
}

QuickDC::QueueFile* QuickDC::QueueManager::getFileBySource(const char* hub, const char* cid) const
{
	for (size_t n = 0; n < files.size(); n++)
	{
		if (files[n]->findSource(hub, cid)) return files[n];
	}
	return 0;
}

QuickDC::QueueFile* QuickDC::QueueManager::first()
{
	fileIterator = files.begin();
	if (fileIterator == files.end()) return 0;
	return *fileIterator;
}

QuickDC::QueueFile* QuickDC::QueueManager::next()
{
	if (fileIterator == files.end()) return 0;

	fileIterator++;
	if (fileIterator == files.end()) return 0;
	return *fileIterator;
}

/* ---- the scheduler API ---- */

void QuickDC::QueueManager::getFilesNeedingSources(std::vector<QueueFile*>& out, size_t minSources) const
{
	for (size_t n = 0; n < files.size(); n++)
	{
		QueueFile* file = files[n];
		if (file->getPriority() == QueueFile::PrioPaused) continue;
		if (file->isComplete()) continue;
		if (file->countSources() >= minSources) continue;
		out.push_back(file);
	}
}

bool QuickDC::QueueManager::getNextTransfer(QueueFile*& file, FileSource*& source)
{
	QueueFile* bestFile = 0;
	FileSource* bestSource = 0;

	for (size_t n = 0; n < files.size(); n++)
	{
		QueueFile* candidate = files[n];
		if (candidate->getPriority() == QueueFile::PrioPaused) continue;
		if (candidate->isComplete()) continue;

		FileSource* idle = candidate->getIdleSource();
		if (!idle) continue;

		if (!bestFile || candidate->getPriority() > bestFile->getPriority())
		{
			bestFile = candidate;
			bestSource = idle;
		}
	}

	if (!bestFile) return false;

	file = bestFile;
	source = bestSource;
	return true;
}

bool QuickDC::QueueManager::reserveChunk(QueueFile* file, FileSource* source, uint64_t& offset, uint64_t& length)
{
	if (!file) return false;

	/* Without a known size there is nothing to divide into chunks yet. */
	if (!file->getSize()) return false;

	if (!file->getChunks().reserve(getChunkSize(), offset, length)) return false;

	if (source) file->setSourceActive(source, true);
	return true;
}

void QuickDC::QueueManager::commitChunk(QueueFile* file, uint64_t offset, uint64_t length)
{
	if (!file) return;

	file->getChunks().commit(offset, length);
	dirty = true;

	QDBG("Committed %d bytes at offset %d of '%s' (%d/%d done)",
		(int) length, (int) offset, file->getLocalFilename(),
		(int) file->getDoneBytes(), (int) file->getSize());

	if (file->isComplete()) onFileComplete(file);
}

void QuickDC::QueueManager::releaseChunk(QueueFile* file, uint64_t offset, uint64_t length)
{
	if (!file) return;
	file->getChunks().release(offset, length);
}

bool QuickDC::QueueManager::getStatus(QueueFile* file, QueueStatus& out) const
{
	if (!file) return false;

	out.filename = file->getLocalFilename();
	out.tth = file->getTTH();
	out.size = file->getSize();
	out.done = file->getDoneBytes();
	out.sources = file->countSources();
	out.activeSources = file->countActiveSources();
	out.priority = file->getPriority();
	out.complete = file->isComplete();

	return true;
}

void QuickDC::QueueManager::getStatus(std::vector<QueueStatus>& out) const
{
	for (size_t n = 0; n < files.size(); n++)
	{
		QueueStatus status;
		if (getStatus(files[n], status)) out.push_back(status);
	}
}

size_t QuickDC::QueueManager::countActiveTransfers() const
{
	size_t count = 0;
	for (size_t n = 0; n < files.size(); n++)
		count += files[n]->countActiveSources();
	return count;
}

uint64_t QuickDC::QueueManager::getChunkSize() const
{
	QuickDC::Core* core = QuickDC::Core::getInstance();
	if (!core || !core->config) return QUICKDC_CHUNK_SIZE;

	core->config->setGroup("Network");
	int configured = core->config->getNumber("Chunk size", QUICKDC_CHUNK_SIZE);
	if (configured <= 0) return QUICKDC_CHUNK_SIZE;

	return (uint64_t) configured;
}

size_t QuickDC::QueueManager::getMaxDownloads() const
{
	QuickDC::Core* core = QuickDC::Core::getInstance();
	if (!core || !core->config) return 3;

	core->config->setGroup("Network");
	int configured = core->config->getNumber("Max downloads", 3);
	if (configured <= 0) return 3;

	return (size_t) configured;
}

/* ---- file lists ---- */

QuickDC::FilelistRequest* QuickDC::QueueManager::requestFilelist(const char* hub, const char* cid)
{
	FilelistRequest* existing = getFilelistRequest(hub, cid);
	if (existing) return existing;

	QDBG("Requesting the file list of '%s' on '%s'", cid ? cid : "", hub ? hub : "");

	FilelistRequest* request = new FilelistRequest(hub, cid);
	filelists.push_back(request);
	return request;
}

QuickDC::FilelistRequest* QuickDC::QueueManager::getFilelistRequest(const char* hub, const char* cid) const
{
	for (size_t n = 0; n < filelists.size(); n++)
	{
		if (filelists[n]->matches(hub, cid)) return filelists[n];
	}
	return 0;
}

QuickDC::FilelistRequest* QuickDC::QueueManager::getFilelistRequest(size_t n) const
{
	if (n >= filelists.size()) return 0;
	return filelists[n];
}

void QuickDC::QueueManager::removeFilelistRequest(FilelistRequest* request)
{
	if (!request) return;

	/* Drop any peer request that was pointing at it. */
	std::vector<std::string> dead;
	for (std::map<std::string, PendingRequest*>::iterator it = pending.begin(); it != pending.end(); it++)
	{
		if ((*it).second->getFilelist() == request) dead.push_back((*it).first);
	}
	for (size_t n = 0; n < dead.size(); n++)
		removeRequest(dead[n].c_str());

	for (std::vector<FilelistRequest*>::iterator it = filelists.begin(); it != filelists.end(); it++)
	{
		if (*it == request)
		{
			filelists.erase(it);
			break;
		}
	}

	delete request;
}

size_t QuickDC::QueueManager::countFilelistRequests() const
{
	return filelists.size();
}

/* ---- token correlation ---- */

std::string QuickDC::QueueManager::makeToken()
{
	char buf[48];
	snprintf(buf, sizeof(buf), "QDC%lu%lu",
		(unsigned long) Samurai::TimeStamp().getInternalData(),
		(unsigned long) ++tokenCounter);
	return std::string(buf);
}

const char* QuickDC::QueueManager::createRequest(QueueFile* file, FileSource* source)
{
	if (!file || !source) return 0;

	std::string token = makeToken();
	PendingRequest* request = new PendingRequest(token.c_str(), file, source);
	pending[token] = request;

	return request->getToken();
}

const char* QuickDC::QueueManager::createRequest(FilelistRequest* filelist)
{
	if (!filelist) return 0;

	std::string token = makeToken();
	PendingRequest* request = new PendingRequest(token.c_str(), filelist);
	pending[token] = request;

	return request->getToken();
}

QuickDC::PendingRequest* QuickDC::QueueManager::lookupRequest(const char* token) const
{
	if (!token) return 0;

	std::map<std::string, PendingRequest*>::const_iterator it = pending.find(std::string(token));
	if (it == pending.end()) return 0;

	return (*it).second;
}

QuickDC::PendingRequest* QuickDC::QueueManager::lookupRequestBySource(const char* hub, const char* cid) const
{
	if (!hub || !cid) return 0;

	for (std::map<std::string, PendingRequest*>::const_iterator it = pending.begin(); it != pending.end(); it++)
	{
		PendingRequest* request = (*it).second;

		if (request->getKind() == PendingRequest::KindFile)
		{
			FileSource* source = request->getSource();
			if (source && source->matches(hub, cid)) return request;
		}
		else
		{
			FilelistRequest* filelist = request->getFilelist();
			if (filelist && filelist->matches(hub, cid)) return request;
		}
	}

	return 0;
}

void QuickDC::QueueManager::onRequestFailed(const char* token)
{
	PendingRequest* request = lookupRequest(token);
	if (!request) return;

	if (request->getKind() == PendingRequest::KindFile)
	{
		QueueFile* file = request->getFile();
		FileSource* source = request->getSource();

		if (file && request->haveReservation())
			file->getChunks().release(request->getReservedOffset(), request->getReservedLength());

		if (source)
		{
			source->onFailure();
			if (file) file->setSourceActive(source, false);
		}
	}
	else if (request->getFilelist())
	{
		/*
		 * Count it, the same way a file source is blamed above. Without this a
		 * peer that always drops the connection would be retried on every tick
		 * for the full lifetime of the request.
		 */
		request->getFilelist()->setActive(false);
		request->getFilelist()->onFailure();
	}

	removeRequest(token);
}

void QuickDC::QueueManager::onRequestFinished(const char* token)
{
	PendingRequest* request = lookupRequest(token);
	if (!request) return;

	if (request->getKind() == PendingRequest::KindFile)
	{
		QueueFile* file = request->getFile();
		FileSource* source = request->getSource();

		if (source)
		{
			source->touch();
			if (file) file->setSourceActive(source, false);
		}
	}

	removeRequest(token);
}

void QuickDC::QueueManager::removeRequest(const char* token)
{
	if (!token) return;

	std::map<std::string, PendingRequest*>::iterator it = pending.find(std::string(token));
	if (it == pending.end()) return;

	delete (*it).second;
	pending.erase(it);
}

void QuickDC::QueueManager::clearRequestsFor(QueueFile* file)
{
	if (!file) return;

	std::vector<std::string> dead;
	for (std::map<std::string, PendingRequest*>::iterator it = pending.begin(); it != pending.end(); it++)
	{
		if ((*it).second->getFile() == file) dead.push_back((*it).first);
	}

	for (size_t n = 0; n < dead.size(); n++)
		removeRequest(dead[n].c_str());
}

/* ---- housekeeping ---- */

void QuickDC::QueueManager::expire()
{
	time_t now = Samurai::TimeStamp().getInternalData();

	/*
	 * Peers that never turned up with the data. A request that holds a
	 * reservation is mid-transfer and may legitimately take a long time, so
	 * only the ones that never got that far are timed out here.
	 */
	std::vector<std::string> dead;
	for (std::map<std::string, PendingRequest*>::iterator it = pending.begin(); it != pending.end(); it++)
	{
		PendingRequest* request = (*it).second;
		if (request->haveReservation()) continue;

		if (now - request->getTimeAdded().getInternalData() > QUICKDC_REQUEST_TIMEOUT)
			dead.push_back((*it).first);
	}
	for (size_t n = 0; n < dead.size(); n++)
	{
		QDBG("A peer never delivered what we asked for; giving up on that request");
		onRequestFailed(dead[n].c_str());
	}

	/* File list requests that went nowhere, or that nobody will serve. */
	std::vector<FilelistRequest*> deadLists;
	for (size_t n = 0; n < filelists.size(); n++)
	{
		if (now - filelists[n]->getTimeAdded().getInternalData() > QUICKDC_FILELIST_TIMEOUT
			|| filelists[n]->getRetryCount() > QUICKDC_FILELIST_MAX_RETRIES)
		{
			deadLists.push_back(filelists[n]);
		}
	}
	for (size_t n = 0; n < deadLists.size(); n++)
	{
		QDBG("Giving up on the file list of '%s' after %d attempt(s)",
			deadLists[n]->getClientID(), (int) deadLists[n]->getRetryCount());
		removeFilelistRequest(deadLists[n]);
	}

	/* Sources that have failed too often, or that we have not seen in weeks. */
	for (size_t n = 0; n < files.size(); n++)
		files[n]->expireSources();
}

void QuickDC::QueueManager::schedule()
{
	QuickDC::Core* core = QuickDC::Core::getInstance();
	if (!core || !core->hubs) return;

	size_t budget = getMaxDownloads();
	size_t inFlight = countActiveTransfers();

	/* A file list we have already asked someone for occupies a slot too. */
	for (std::map<std::string, PendingRequest*>::const_iterator it = pending.begin(); it != pending.end(); it++)
	{
		if ((*it).second->getKind() == PendingRequest::KindFilelist) inFlight++;
	}

	if (inFlight >= budget) return;

	/*
	 * File lists first. They are small, someone is usually waiting to look at
	 * one, and driving them from here rather than from whoever asked means a
	 * connection that cannot be made now is retried instead of lost.
	 */
	for (size_t n = 0; n < filelists.size() && inFlight < budget; n++)
	{
		FilelistRequest* list = filelists[n];

		/* Already being received. */
		if (list->isActive()) continue;

		/* Already asked; expire() frees it again if it comes to nothing. */
		if (lookupRequestBySource(list->getHubUrl(), list->getClientID())) continue;

		/* Back off between attempts; this runs several times a second. */
		if (!list->isReadyToRetry()) continue;

		QuickDC::Hub* hub = core->hubs->getHubByUrl(list->getHubUrl());
		if (!hub) continue;   /* not logged in there at the moment */

		const char* created = createRequest(list);
		if (!created) continue;

		std::string token(created);
		list->onAttempt();

		if (!hub->connectUserByClientID(list->getClientID(), token.c_str()))
		{
			/* Usually means they have left; expire() gives up after a few. */
			removeRequest(token.c_str());
			list->onFailure();
			continue;
		}

		QDBG("Asked '%s' for their file list", list->getClientID());
		inFlight++;
	}

	/* Highest priority first. PrioPaused is never scheduled. */
	for (int prio = (int) QueueFile::PrioHigh; prio >= (int) QueueFile::PrioLow; prio--)
	{
		for (size_t n = 0; n < files.size() && inFlight < budget; n++)
		{
			QueueFile* file = files[n];
			if ((int) file->getPriority() != prio) continue;
			if (file->isComplete()) continue;
			if (!file->getSize()) continue;

			for (size_t s = 0; s < file->countSources() && inFlight < budget; s++)
			{
				FileSource* source = file->getSource(s);
				if (!source || source->isExpired()) continue;
				if (file->isSourceActive(source)) continue;

				QuickDC::Hub* hub = core->hubs->getHubByUrl(source->getHubUrl());
				if (!hub) continue;   /* not logged in there at the moment */

				const char* created = createRequest(file, source);
				if (!created) continue;

				std::string token(created);
				if (!hub->connectUserByClientID(source->getClientID(), token.c_str()))
				{
					removeRequest(token.c_str());
					continue;
				}

				/* Claim the source now, so we do not ask it twice. */
				file->setSourceActive(source, true);
				inFlight++;
			}
		}
	}
}

void QuickDC::QueueManager::onFileComplete(QueueFile* file)
{
	if (!file) return;

	if (!file->getTTH())
	{
		/* Nothing to check it against, so take it at face value. */
		QDBG("Download complete (unverified): %s", file->getLocalFilename());

		Samurai::IO::File part(file->getTempFilename());
		if (!part.rename(std::string(file->getLocalFilename())))
			QERR("Unable to rename '%s' to '%s'", file->getTempFilename(), file->getLocalFilename());

		remove(file);
		return;
	}

	QDBG("All chunks downloaded for '%s'; verifying the hash", file->getLocalFilename());

	QuickDC::Core* core = QuickDC::Core::getInstance();
	if (!core || !core->hash)
	{
		QERR("No hash manager available; cannot verify '%s'", file->getLocalFilename());
		return;
	}

	core->hash->addJob(Samurai::IO::File(file->getTempFilename()), verifier);
}

size_t QuickDC::QueueManager::collectSources(::Share::RemoteDir* dir, const char* hub, const char* cid)
{
	if (!dir || !hub || !cid) return 0;

	size_t added = 0;

	for (size_t n = 0; n < dir->countEntries(); n++)
	{
		::Share::RemoteFile* entry = dir->getEntry(n);
		if (!entry || !entry->getTTH()) continue;

		/*
		 * Matched on the TTH, not the name: the same file is routinely called
		 * something else on someone else's disk.
		 */
		QueueFile* file = getFileByTTH(entry->getTTH());
		if (!file) continue;
		if (file->findSource(hub, cid)) continue;

		file->addSource(new FileSource(hub, cid, entry->getName()));
		added++;
	}

	for (size_t n = 0; n < dir->countSubdirs(); n++)
		added += collectSources(dir->getSubdir(n), hub, cid);

	return added;
}

size_t QuickDC::QueueManager::onFilelistDownloaded(FilelistRequest* request, const char* hub, const char* cid)
{
	if (!request) return 0;

	Samurai::IO::File raw(request->getTempFilename());
	if (!raw.exists())
	{
		QERR("The file list was supposed to be at '%s'", request->getTempFilename());
		return 0;
	}

	/*
	 * Neither protocol says whether a peer answers with files.xml or
	 * files.xml.bz2, so look at what actually turned up. "BZh" is the bzip2
	 * magic number.
	 */
	bool compressed = false;
	size_t size = (size_t) raw.size();

	if (size > 3 && raw.open(Samurai::IO::File::Read))
	{
		char magic[3];
		compressed = (raw.read(magic, 3) == 3 && !memcmp(magic, "BZh", 3));
		raw.close();
	}

	if (!compressed)
	{
		Samurai::IO::File target(request->getLocalFilename());
		target.remove();

		if (!raw.rename(std::string(request->getLocalFilename())))
		{
			QERR("Unable to move the file list into place");
			return 0;
		}
	}
	else
	{
#ifdef BZIP2
		QDBG("The file list is bzip2 encoded; unpacking it");

		if (!raw.open(Samurai::IO::File::Read))
		{
			QERR("Unable to reopen the downloaded file list");
			return 0;
		}

		char* packed = new char[size];
		ssize_t got = raw.read(packed, size);
		raw.close();

		if (got <= 0)
		{
			QERR("Unable to read the downloaded file list");
			delete[] packed;
			return 0;
		}

		Samurai::IO::BZip2Decompressor codec;
		Samurai::IO::Buffer plain;

		char out[65536];
		char* in = packed;
		size_t left = (size_t) got;

		while (left)
		{
			size_t consumed = left;
			size_t produced = sizeof(out);

			if (!codec.exec(in, consumed, out, produced))
			{
				QERR("The file list is not valid bzip2 data");
				delete[] packed;
				return 0;
			}

			plain.append(out, produced);
			in += consumed;
			left -= consumed;

			/* No forward progress means the stream is truncated. */
			if (!consumed && !produced) break;
		}
		delete[] packed;

		Samurai::IO::File target(request->getLocalFilename());
		if (!target.open(Samurai::IO::File::Write | Samurai::IO::File::Truncate))
		{
			QERR("Unable to write the unpacked file list");
			return 0;
		}

		size_t length = plain.size();
		char* data = new char[length ? length : 1];
		plain.pop(data, length);
		target.write(data, length);
		target.close();
		delete[] data;

		raw.remove();
#else
		QERR("The file list is bzip2 encoded, but bzip2 support is not built in");
		return 0;
#endif
	}

	/* Read it, and mine it for anything the queue is waiting for. */
	Samurai::IO::File listfile(request->getLocalFilename());
	::Share::RemoteFileList decoder(&listfile);

	if (!decoder.decode())
	{
		QERR("Unable to decode the file list of '%s'", request->getClientID());
		return 0;
	}

	size_t added = collectSources(decoder.root, hub, cid);

	QDBG("Read the file list of '%s'; it gave us %d new source(s)",
		request->getClientID(), (int) added);

	return added;
}

void QuickDC::QueueManager::onFileVerified(const char* tempname, const char* tth)
{
	if (!tempname || !tth) return;

	Verification result;
	result.tempname = tempname;
	result.tth = tth;
	verifications.push_back(result);
}

void QuickDC::QueueManager::processVerifications()
{
	if (verifications.empty()) return;

	std::vector<Verification> work;
	work.swap(verifications);

	for (size_t n = 0; n < work.size(); n++)
	{
		QueueFile* file = 0;
		for (size_t i = 0; i < files.size(); i++)
		{
			if (!strcmp(files[i]->getTempFilename(), work[n].tempname.c_str()))
			{
				file = files[i];
				break;
			}
		}

		if (!file) continue;   /* no longer queued */

		const char* expected = file->getTTH();
		if (expected && work[n].tth == expected)
		{
			QDBG("Download verified: %s", file->getLocalFilename());

			Samurai::IO::File part(file->getTempFilename());
			if (part.rename(std::string(file->getLocalFilename())))
			{
				remove(file);
			}
			else
			{
				QERR("Unable to rename '%s' to '%s'",
					file->getTempFilename(), file->getLocalFilename());
			}
			continue;
		}

		/*
		 * Some source fed us bad data. Without the tthl leaf hashes there is
		 * no way to tell which chunk was at fault, so the whole file has to
		 * be fetched again.
		 */
		QERR("Hash mismatch for '%s': expected '%s' but got '%s'. Starting over.",
			file->getLocalFilename(), expected ? expected : "", work[n].tth.c_str());

		file->getChunks().clear();
		file->getChunks().clearReservations();

		Samurai::IO::File part(file->getTempFilename());
		part.remove();

		dirty = true;
	}
}

/* ---- persistence ---- */

bool QuickDC::QueueManager::load()
{
	Samurai::IO::File file(QUICKDC_QUEUE_FILE);

	if (!file.exists())
	{
		QDBG("No download queue on disk yet");
		return true;
	}

	size_t size = (size_t) file.size();
	if (!size)
	{
		QDBG("The download queue on disk is empty");
		return true;
	}

	if (!file.open(Samurai::IO::File::Read))
	{
		QERR("Unable to open '%s' for reading", QUICKDC_QUEUE_FILE);
		return false;
	}

	char* temp = new char[size];
	ssize_t got = file.read(temp, size);
	file.close();

	if (got <= 0)
	{
		QERR("Unable to read the download queue");
		delete[] temp;
		return false;
	}

	Samurai::IO::Buffer buffer((size_t) got);
	buffer.append(temp, (size_t) got);
	delete[] temp;

	QueueXMLReader handler(this, &buffer);
	bool ret = handler.parse();

	/* Nothing can have been in flight when this was written. */
	for (size_t n = 0; n < files.size(); n++)
		files[n]->getChunks().clearReservations();

	dirty = false;
	lastSaved.reset();

	QDBG("Loaded %d file(s) into the download queue", (int) files.size());
	return ret;
}

bool QuickDC::QueueManager::save()
{
	if (!dirty) return true;

	Samurai::IO::Buffer* xmlout = new Samurai::IO::Buffer();

	xmlout->append("<?xml version=\"1.1\" encoding=\"utf-8\" standalone=\"yes\"?>" XML_NEWLINE);
	xmlout->append("<DownloadQueue version=\"1\" generator=\"" PRODUCT "/" VERSION " (" SYSTEM "/" CPU ")\" updated=\"");
	xmlout->append(Samurai::TimeStamp().getTime("%s"));
	xmlout->append("\">" XML_NEWLINE);

	size_t stored = 0;
	for (size_t n = 0; n < files.size(); n++)
	{
		QueueFile* file = files[n];

		if (!file->isPersistent())
		{
			QDBG("Not storing '%s': %s", file->getLocalFilename(),
				file->isComplete() ? "already complete" : "no usable TTH or size");
			continue;
		}

		xmlout->append(XML_INDENT "<File name=\"");
		XML::escape(xmlout, file->getLocalFilename());
		xmlout->append("\" size=\"");
		xmlout->append(quickdc_ulltoa(file->getSize()));
		xmlout->append("\" tth=\"");
		XML::escape(xmlout, file->getTTH());
		xmlout->append("\" added=\"");
		xmlout->append(file->added.getTime("%s"));
		xmlout->append("\" priority=\"");
		xmlout->append(QueueFile::convertPriority(file->getPriority()));
		xmlout->append("\">" XML_NEWLINE);

		for (size_t s = 0; s < file->countSources(); s++)
		{
			FileSource* source = file->getSource(s);
			char* url = source->toURL();
			if (!url) continue;

			xmlout->append(XML_INDENT XML_INDENT "<Source url=\"");
			XML::escape(xmlout, url);
			xmlout->append("\" lastseen=\"");
			xmlout->append(quickdc_ulltoa((uint64_t) source->getLastSeen().getInternalData()));
			xmlout->append("\" retries=\"");
			xmlout->append(quickdc_ulltoa((uint64_t) source->getRetryCount()));
			xmlout->append("\" />" XML_NEWLINE);

			free(url);
		}

		uint64_t start = 0;
		uint64_t end = 0;
		for (size_t c = 0; file->getChunks().getRange(c, start, end); c++)
		{
			xmlout->append(XML_INDENT XML_INDENT "<Chunk start=\"");
			xmlout->append(quickdc_ulltoa(start));
			xmlout->append("\" end=\"");
			xmlout->append(quickdc_ulltoa(end));
			xmlout->append("\" />" XML_NEWLINE);
		}

		xmlout->append(XML_INDENT "</File>" XML_NEWLINE);
		stored++;
	}

	xmlout->append("</DownloadQueue>" XML_NEWLINE);

	/* Write beside the real file, then move it into place. */
	bool ret = false;
	Samurai::IO::File out(QUICKDC_QUEUE_FILE_NEW);

	if (out.open(Samurai::IO::File::Write | Samurai::IO::File::Truncate))
	{
		size_t length = xmlout->size();
		char* data = new char[length];
		xmlout->pop(data, length);

		ssize_t written = out.write(data, length);
		out.close();
		delete[] data;

		if (written == (ssize_t) length)
		{
			Samurai::IO::File target(QUICKDC_QUEUE_FILE);
			target.remove();

			ret = out.rename(std::string(QUICKDC_QUEUE_FILE));
			if (!ret)
				QERR("Unable to move '%s' into place", QUICKDC_QUEUE_FILE_NEW);
		}
		else
		{
			QERR("Unable to write the download queue");
		}
	}
	else
	{
		QERR("Unable to open '%s' for writing", QUICKDC_QUEUE_FILE_NEW);
	}

	delete xmlout;

	if (ret)
	{
		QDBG("Saved %d file(s) to the download queue", (int) stored);
		dirty = false;
		lastSaved.reset();
	}

	return ret;
}
