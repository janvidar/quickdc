/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/downloadqueue.h"
#include "network/filesource.h"
#include "network/leafhashes.h"
#include "network/hubmanager.h"
#include "api/core.h"
#include "api/hub.h"
#include "config/preferences.h"
#include "hash/hashmanager.h"
#include "hash/hashjob.h"
#include <samurai/io/xml.h>
#include "share/dcfilelist.h"
#include "share/searchrequest.h"

#include <samurai/util/base32.h>
#include <samurai/util/random.h>
#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <samurai/io/compression.h>

#include <algorithm>
#include <span>
#include <string_view>
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
	: filename(filename_ ? filename_ : "")
	, tempname(filename + QUICKDC_PART_SUFFIX)
	, tth(tth_ ? tth_ : "")
	, size(size_)
	, priority(PrioNormal)
	, chunks(size_)
	, leafHashes(0)
{
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

	delete leafHashes;
}

char* QuickDC::QueueFile::makeLeafFilename() const
{
	return LeafHashes::makeFilename(tempname.c_str());
}

bool QuickDC::QueueFile::loadLeaves(const char* data, size_t length)
{
	if (tth.empty() || !size) return false;

	LeafHashes* fresh = new LeafHashes();
	if (!fresh->load(tth.c_str(), size, data, length))
	{
		delete fresh;
		return false;
	}

	delete leafHashes;
	leafHashes = fresh;

	/* Keep them, so a restart does not have to ask a peer again. */
	char* name = makeLeafFilename();
	if (name)
	{
		leafHashes->save(name);
		free(name);
	}
	return true;
}

bool QuickDC::QueueFile::loadLeavesFromDisk()
{
	if (leafHashes || tth.empty() || !size) return false;

	char* name = makeLeafFilename();
	if (!name) return false;

	LeafHashes* fresh = new LeafHashes();
	bool ok = fresh->load(name, tth.c_str(), size);
	free(name);

	if (!ok)
	{
		delete fresh;
		return false;
	}

	leafHashes = fresh;
	return true;
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
	/* An absent attribute is not an unknown priority, and there is no view to
	   compare for it either; both mean the default. */
	if (!str) return PrioNormal;

	const std::string_view name(str);
	if (Samurai::Util::iequals(name, "paused")) return PrioPaused;
	if (Samurai::Util::iequals(name, "low"))    return PrioLow;
	if (Samurai::Util::iequals(name, "high"))   return PrioHigh;
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
		QDBG("Forgetting a stale source for '%s'", filename.c_str());
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

void QuickDC::QueueFile::searchOnlyOn(const char* hubUrl)
{
	if (hubUrl) only_hub = hubUrl;
	else only_hub.clear();
}

const char* QuickDC::QueueFile::searchesOnlyOn() const
{
	return only_hub.empty() ? 0 : only_hub.c_str();
}

bool QuickDC::QueueFile::searchedWithin(time_t seconds)
{
	if (!ever_searched) return false;

	return searched.elapsed() < seconds;
}

void QuickDC::QueueFile::noteSearched()
{
	ever_searched = true;
	searched.reset();
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
	if (tth.size() != QUICKDC_TTH_LENGTH) return false;
	if (!size) return false;
	return !isComplete();
}

/* ------------------------------------------------------------------------- */
/* FilelistRequest                                                           */
/* ------------------------------------------------------------------------- */

bool QuickDC::FilelistRequest::isValidIdentifier(const char* id)
{
	if (!id || !*id) return false;

	const std::string_view name(id);
	if (name.size() > QUICKDC_IDENTIFIER_MAX) return false;

	/* A single dot names the directory itself, and two of them anywhere leave
	   it - so neither can be part of a name we make up. */
	if (name == ".") return false;
	if (name.find("..") != std::string_view::npos) return false;

	for (const char c : name)
	{
		const unsigned char ch = static_cast<unsigned char>(c);

		/* A separator would make the identifier a path of its own, and a
		   control byte has no business in a nickname whatever it does to a
		   file system. ':' names a drive or an alternate stream elsewhere. */
		if (ch == '/' || ch == '\\' || ch == ':') return false;
		if (ch < 0x20 || ch == 0x7f) return false;
	}

	return true;
}

QuickDC::FilelistRequest::FilelistRequest(const char* hub_, const char* cid_, const char* dir_)
	: hub(hub_ ? hub_ : "")
	, cid(cid_ ? cid_ : "")
	, retryCount(0)
	, active(false)
{
	/*
	 * Checked here rather than in whoever asked, because this is the only place
	 * the wire string becomes a path and the callers are protocol code and test
	 * programs. A refused identifier leaves the request without a filename,
	 * which isValid() reports and requestFilelist() acts on.
	 */
	if (!isValidIdentifier(cid.c_str()))
	{
		QERR("Refusing a file list request: '%s' cannot name a file", cid.c_str());
		return;
	}

	filename = std::string(dir_ ? dir_ : QUICKDC_LIST_DIR) + '/' + cid + ".xml";
	tempname = filename + QUICKDC_PART_SUFFIX;
}

QuickDC::FilelistRequest::~FilelistRequest()
{
}

bool QuickDC::FilelistRequest::matches(const char* hub_, const char* cid_) const
{
	if (!hub_ || !cid_) return false;
	return (hub == hub_ && cid == cid_);
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
	: token(token_ ? token_ : "")
	, kind(KindFile)
	, file(file_)
	, source(source_)
	, filelist(0)
	, asked(std::chrono::steady_clock::now())
	, reserved(false)
	, offset(0)
	, length(0)
{

}

QuickDC::PendingRequest::PendingRequest(const char* token_, FilelistRequest* filelist_)
	: token(token_ ? token_ : "")
	, kind(KindFilelist)
	, file(0)
	, source(0)
	, filelist(filelist_)
	, asked(std::chrono::steady_clock::now())
	, reserved(false)
	, offset(0)
	, length(0)
{

}

QuickDC::PendingRequest::~PendingRequest()
{
}

double QuickDC::PendingRequest::elapsedSeconds() const
{
	const std::chrono::duration<double> spent =
		std::chrono::steady_clock::now() - asked;

	return spent.count();
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
		void EventFileStarted(const QuickDC::Hash::Job*) override { }
		void EventFileQueued(const QuickDC::Hash::Job*) override { }
		void EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t, uint64_t) override { }

		void EventFileHashed(const QuickDC::Hash::Job* job) override
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

class QueueXMLReader : public Samurai::IO::XmlContentHandler
{
	public:
		QueueXMLReader(QueueManager* queue_, Samurai::IO::Buffer* buffer)
			: source(buffer)
			, queue(queue_)
			, current(0)
			, broken(false)
			, dropped(0)
		{
		}

		~QueueXMLReader() override
		{
			delete current;
		}

		bool parse()
		{
			if (!source) return false;

			/*
			 * Our own file, written by the queue below - so the bounds are what
			 * this writes rather than what a stranger might send, and a file that
			 * has grown past them is one to refuse rather than to trust.
			 */
			Samurai::IO::XmlReader::Limits limits;
			limits.maxDocumentSize = QUICKDC_QUEUE_MAX_SIZE;
			limits.maxDepth        = 8;
			limits.maxElements     = QUICKDC_QUEUE_MAX_ELEMENTS;
			limits.maxAttributes   = 32;
			limits.maxTextLength   = 4096;

			const Samurai::IO::XmlError parsed = Samurai::IO::XmlReader::parse(
				std::string_view(source->ptr(), source->size()), *this, limits);

			if (parsed != Samurai::IO::XmlError::Ok)
			{
				QERR("queue.xml: %s", Samurai::IO::toString(parsed));
				broken = true;
			}

			if (dropped)
				QERR("Discarded %d unusable entry/entries while loading the download queue",
					static_cast<int>(dropped));

			return !broken;
		}

	public:
		void characters(std::string_view) override { }

		void startElement(std::string_view, std::string_view element,
			const Attributes& attributes) override
		{
			if (Samurai::Util::iequals(element, "File"))
				startFile(attributes);
			else if (Samurai::Util::iequals(element, "Source"))
				startSource(attributes);
			else if (Samurai::Util::iequals(element, "Chunk"))
				startChunk(attributes);
		}

		void endElement(std::string_view, std::string_view element) override
		{
			if (!Samurai::Util::iequals(element, "File")) return;

			if (current)
			{
				queue->add(current);
				current = 0;
			}
		}

	protected:
		/*
		 * A complaint about the contents rather than about the markup: the entry
		 * is dropped and the rest of the file is still read, because one unusable
		 * entry is not a reason to lose a queue.
		 */
		void error(const char* str) { QERR("queue.xml: %s", str); }

		static const char* getAttribute(const Attributes& list, std::string_view name)
		{
			for (size_t n = 0; n < list.size(); n++)
			{
				if (Samurai::Util::iequals(list[n].first, name)) return list[n].second.c_str();
			}
			return 0;
		}

		void startFile(const Attributes& list)
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

			if (!name || !*name || !s_size || !*s_size || !tth || !*tth)
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

			uint64_t size = Samurai::Util::Convert::to_uint64(s_size);
			if (!size)
			{
				error("<File> has a zero or unreadable size");
				dropped++;
				return;
			}

			current = new QueueFile(name, tth, size);
			current->setPriority(QueueFile::convertPriority(prio));

			if (s_added)
				current->added = Samurai::TimeStamp(static_cast<time_t>(Samurai::Util::Convert::to_int64(s_added)));
		}

		void startSource(const Attributes& list)
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
				source->setRetryCount(static_cast<size_t>(Samurai::Util::Convert::to_int32(s_retries)));
			if (s_lastseen)
				source->setLastSeen(Samurai::TimeStamp(static_cast<time_t>(Samurai::Util::Convert::to_int64(s_lastseen))));

			if (source->isExpired())
			{
				/* Failed too often, or not seen for too long, to be worth keeping. */
				delete source;
				return;
			}

			current->addSource(source);
		}

		void startChunk(const Attributes& list)
		{
			if (!current) return;

			const char* s_start = getAttribute(list, "start");
			const char* s_end = getAttribute(list, "end");

			if (!s_start || !*s_start || !s_end || !*s_end)
			{
				error("<Chunk> is missing start or end");
				return;
			}

			uint64_t start = Samurai::Util::Convert::to_uint64(s_start);
			uint64_t end = Samurai::Util::Convert::to_uint64(s_end);

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
		Samurai::IO::Buffer* source;
		QueueManager* queue;
		QueueFile* current;
		bool broken;
		size_t dropped;
};

}

/* ------------------------------------------------------------------------- */
/* QueueManager                                                              */
/* ------------------------------------------------------------------------- */

namespace {

/*
 * Whether this is a tiger tree root and nothing else.
 *
 * A hash handed to us by a user interface, or read out of a peer's bulletin
 * board index, goes on the wire in a search - so it is checked here rather than
 * left to come up empty against the queue. The length alone is not enough: a
 * string of the right length that is not base32 cannot be any file's root, and
 * a hub is no place to find that out.
 */
bool isTigerRoot(const char* tth)
{
	if (!tth || strlen(tth) != QUICKDC_TTH_LENGTH) return false;

	for (const char* p = tth; *p; p++)
	{
		if ((*p >= 'A' && *p <= 'Z') || (*p >= '2' && *p <= '7')) continue;
		return false;
	}
	return true;
}

/*
 * Whether these two name a peer we are able to record.
 *
 * FileSource keeps whatever hub and client ID it is handed without looking at
 * them, and an empty one of either counts as valid there - two empty ones then
 * match each other, so a source naming nobody would be taken for one we already
 * have. The rest is what the queue file demands: a source is written out as
 * {hub}/{cid}/TTH:{root} and read back by splitting on the slashes, so a client
 * ID containing one comes back as a different user with a filename, a hub URL
 * containing one loses its tail the same way, and one without a protocol does
 * not come back at all.
 */
bool isRecordablePeer(const char* hubUrl, const char* cid)
{
	if (!hubUrl || !cid || !*hubUrl || !*cid) return false;

	const char* authority = strstr(hubUrl, "://");
	if (!authority || authority == hubUrl) return false;
	if (!authority[3] || strchr(authority + 3, '/')) return false;

	return strchr(cid, '/') == 0;
}

/**
 * Whether {hub, cid} is this client's own session on that hub.
 *
 * A hub we are not logged in to answers false, which is the safe way round: this is
 * asked to refuse something, and "we cannot tell" must never refuse a download.
 */
bool isOurselves(const QuickDC::Core* core, const char* hubUrl, const char* cid)
{
	if (!core || !core->hubs || !hubUrl || !cid) return false;

	QuickDC::Hub* hub = core->hubs->getHubByUrl(hubUrl);

	return hub && hub->isLocalUser(cid);
}

}

QuickDC::QueueManager::QueueManager(Core* core_, const char* path_)
	: core(core_)
	, verifier(0)
	, dirty(false)
	, tokenCounter(0)
	, lastSourceSearch(0)
	, searchRotation(0)
	, lastBurstSearch(0)
	, burstAllowance(QUICKDC_SOURCE_SEARCH_BURST)
	, path(path_ ? path_ : "")
	, pathNew(path + ".new")
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
	searchForSources();

	if (dirty && lastSaved.elapsed() >= QUICKDC_QUEUE_SAVE_INTERVAL)
		save();
}

enum QuickDC::QueueManager::SourceSearchResult QuickDC::QueueManager::sendSourceSearch(QueueFile* file)
{
	/* Only a hash can find a source; a name is whatever it is called here. */
	if (!file || file->tth.size() != QUICKDC_TTH_LENGTH) return SourceSearchBadHash;

	if (!core || !core->hubs) return SourceSearchNotSent;

	const std::string suffix = makeToken();
	if (suffix.empty()) return SourceSearchNotSent;

	std::string token("src");
	token += suffix;

	QuickDC::Share::SearchRequest request(QuickDC::Share::SearchRequest::SizeAny,
		0, file->tth.c_str(), token.c_str(), 0);

	/*
	 * Where the file names a hub, that hub and no other; see
	 * QueueFile::searchOnlyOn(). A hub this client is not logged in to answers
	 * nothing, and is reported as not sent rather than as sent to nobody, so the
	 * rotation keeps its turn and tries again once the hub is back.
	 */
	const size_t sent = file->searchesOnlyOn()
		? core->hubs->searchOneHub(file->searchesOnlyOn(), &request)
		: core->hubs->searchAllHubs(&request);

	if (!sent)
	{
		/*
		 * Said, because a file confined to a hub this client is not on is looked for
		 * nowhere at all, and silence would leave that looking like a peer that never
		 * answered. Answered apart from NotSent as well, so that one such file does not
		 * stop the rotation asking about any of the others.
		 */
		if (file->searchesOnlyOn())
		{
			QDBG("Not asking about '%s' yet: not logged in to %s",
				file->getLocalFilename(), file->searchesOnlyOn());

			return SourceSearchHubAbsent;
		}

		return SourceSearchNotSent;
	}

	/* So that whichever of the two paths sent it, the other does not repeat it. */
	file->noteSearched();

	QDBG("Looking for more sources for '%s' on %s; it has %d",
		file->getLocalFilename(),
		file->searchesOnlyOn() ? file->searchesOnlyOn() : "every hub",
		static_cast<int>(file->countSources()));
	return SourceSearchSent;
}

void QuickDC::QueueManager::searchForSources()
{
	if (lastSourceSearch.elapsed() < QUICKDC_SOURCE_SEARCH_INTERVAL) return;

	std::vector<QueueFile*> wanted;
	getFilesNeedingSources(wanted, QUICKDC_SOURCE_SEARCH_MIN_SOURCES);
	if (wanted.empty()) return;

	/* One file per interval, taken in turn. */
	for (size_t n = 0; n < wanted.size(); n++)
	{
		size_t index = (searchRotation + n) % wanted.size();

		/*
		 * Not one somebody has just asked about out of turn. The burst spends its own
		 * allowance and leaves this interval alone, which is what makes an urgent ask
		 * urgent - but it does not make the file need asking about again, and at
		 * start-up this interval has always elapsed, so without this the first file
		 * of every session is searched for twice within the second.
		 */
		if (wanted[index]->searchedWithin(QUICKDC_SOURCE_SEARCH_INTERVAL)) continue;

		const enum SourceSearchResult sent = sendSourceSearch(wanted[index]);

		/*
		 * Nowhere to send anything yet; retry rather than lose the interval. Only for
		 * no hub at all: a file whose own hub is absent answers HubAbsent instead and
		 * falls to the next line, because stopping the pass for it would leave one such
		 * file - a post from a hub the user has left - holding up the source search for
		 * every other download in the queue, on every tick, for as long as it is queued.
		 */
		if (sent == SourceSearchNotSent) return;

		/* Nothing to search by, or nowhere for this one file; not its turn after all. */
		if (sent != SourceSearchSent) continue;

		searchRotation = index + 1;
		lastSourceSearch.reset();
		return;
	}
}

bool QuickDC::QueueManager::takeSearchFromBurst()
{
	const time_t since = lastBurstSearch.elapsed();

	/*
	 * Whole searches only, so that a caller asking every second is credited at
	 * the interval rather than continuously. A clock that went backwards credits
	 * nothing, which costs a caller one interval of waiting at worst.
	 */
	if (since >= QUICKDC_SOURCE_SEARCH_BURST_REFILL)
	{
		const size_t credit = static_cast<size_t>(since / QUICKDC_SOURCE_SEARCH_BURST_REFILL);
		burstAllowance = std::min<size_t>(QUICKDC_SOURCE_SEARCH_BURST, burstAllowance + credit);
	}

	if (!burstAllowance) return false;

	burstAllowance--;
	lastBurstSearch.reset();
	return true;
}

bool QuickDC::QueueManager::requestSourceSearch(const char* tth, SourceSearchResult* why)
{
	/* Which is the answer for a hash we never got as far as looking up. */
	enum SourceSearchResult outcome = SourceSearchBadHash;

	if (isTigerRoot(tth))
	{
		QueueFile* file = getFileByTTH(tth);

		/*
		 * The burst is charged here rather than around the send, so that asking
		 * is what costs; a caller which cannot be answered yet is bounded too.
		 */
		if (!file) outcome = SourceSearchNotQueued;
		else if (!takeSearchFromBurst()) outcome = SourceSearchRateLimited;
		else outcome = sendSourceSearch(file);
	}

	if (why) *why = outcome;
	return outcome == SourceSearchSent;
}

bool QuickDC::QueueManager::addKnownSource(const char* tth, const char* hubUrl, const char* cid)
{
	if (!isTigerRoot(tth) || !isRecordablePeer(hubUrl, cid)) return false;

	/* The author of a post is a known source for it, and sometimes that is us. */
	if (isOurselves(core, hubUrl, cid)) return false;

	QueueFile* file = getFileByTTH(tth);
	if (!file) return false;

	/* Keeping the one we have; it carries the retry count and the last seen. */
	if (file->findSource(hubUrl, cid)) return false;

	std::string name("TTH:");
	name += tth;
	file->addSource(new FileSource(hubUrl, cid, name.c_str()));

	/* addSource() drops what it will not have, so ask whether it took it. */
	if (!file->findSource(hubUrl, cid)) return false;

	if (file->isPersistent()) dirty = true;

	QDBG("'%s' is a known source for '%s'", cid, file->getLocalFilename());
	return true;
}

/* ---- contents ---- */

QuickDC::Subscription QuickDC::QueueManager::subscribe(QueueListener* listener)
{
	return listeners.subscribe(listener);
}

void QuickDC::QueueManager::addListener(QueueListener* listener)
{
	listeners.add(listener);
}

void QuickDC::QueueManager::removeListener(QueueListener* listener)
{
	listeners.remove(listener);
}

QuickDC::QueueFile* QuickDC::QueueManager::add(const char* filename, const char* tth, uint64_t size)
{
	QueueFile* existing = getFileByName(filename);
	if (existing)
	{
		/* A search reply may know things the original request did not. */
		if (!existing->getSize() && size)
			existing->setSize(size);

		if (existing->tth.empty() && tth)
		{
			existing->tth = tth;
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

	/* Needed to answer a PSR for a file we are not downloading right now. */
	file->loadLeavesFromDisk();

	if (file->isPersistent()) dirty = true;

	listeners.notify([file](QueueListener& l) { l.EventQueueFileAdded(file); });
}

void QuickDC::QueueManager::remove(QueueFile* file)
{
	if (!file) return;

	QDBG("Removing file from download queue: %s", file->getLocalFilename());

	clearRequestsFor(file);

	/* The leaves were only ever a means to an end. */
	char* leafname = file->makeLeafFilename();
	if (leafname)
	{
		Samurai::IO::File(leafname).remove();
		free(leafname);
	}

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

	/* Last thing before it goes, so a listener can still read it. */
	listeners.notify([file](QueueListener& l) { l.EventQueueFileRemoved(file); });

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
		if (!files[n]->tth.empty() && files[n]->tth == tth) return files[n];
	}
	return 0;
}

QuickDC::QueueFile* QuickDC::QueueManager::getFileByName(const char* localFilename) const
{
	if (!localFilename) return 0;

	for (size_t n = 0; n < files.size(); n++)
	{
		if (files[n]->filename == localFilename) return files[n];
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

void QuickDC::QueueManager::onPartialSources(const char* hubUrl, const char* id, const char* tth, const char* indexes)
{
	if (!hubUrl || !id || !tth || !indexes) return;

	QueueFile* file = getFileByTTH(tth);
	if (!file) return;

	FileSource* source = file->findSource(hubUrl, id);
	if (!source)
	{
		std::string name("TTH:");
		name += tth;
		file->addSource(new FileSource(hubUrl, id, name.c_str()));
		source = file->findSource(hubUrl, id);
	}

	if (!source) return;

	source->setAvailability(indexes);
	QDBG("'%s' has part of '%s': %s", id, file->getLocalFilename(), indexes);
}

Samurai::IO::File* QuickDC::QueueManager::openLeafFile(const char* tth, uint64_t& length)
{
	QueueFile* file = getFileByTTH(tth);
	if (!file) return 0;

	char* name = file->makeLeafFilename();
	if (!name) return 0;

	Samurai::IO::File* leaves = new Samurai::IO::File(name);
	free(name);

	if (!leaves->exists() || !leaves->size())
	{
		delete leaves;
		return 0;
	}

	length = static_cast<uint64_t>(leaves->size());
	QDBG("Serving the leaf hashes of a file we are downloading");
	return leaves;
}

Samurai::IO::File* QuickDC::QueueManager::openVerifiedRange(const char* tth, uint64_t offset, int64_t length, uint64_t& servable)
{
	QueueFile* file = getFileByTTH(tth);
	if (!file) return 0;

	/* Only what the leaves have vouched for may go out. */
	LeafHashes* leaves = file->getLeaves();
	if (!leaves || !leaves->isLoaded()) return 0;

	uint64_t runEnd = file->getChunks().getRunEnd(offset);
	if (runEnd <= offset)
	{
		QDBG("Asked for %d of '%s', which we do not have", static_cast<int>(offset), tth);
		return 0;
	}

	if (length >= 0 && offset + static_cast<uint64_t>(length) > runEnd)
	{
		QDBG("Asked for more of '%s' than we have verified", tth);
		return 0;
	}

	Samurai::IO::File* part = new Samurai::IO::File(file->getTempFilename());
	if (!part->exists())
	{
		delete part;
		return 0;
	}

	servable = runEnd;
	QDBG("Serving %d-%d of a file we are downloading", static_cast<int>(offset), static_cast<int>(runEnd));
	return part;
}

bool QuickDC::QueueManager::sourceIsUseful(QueueFile* file, FileSource* source) const
{
	if (!file || !source) return false;
	if (!source->isPartial()) return true;

	LeafHashes* leaves = file->getLeaves();
	if (!leaves || !leaves->isLoaded()) return true;

	ChunkMap theirs;
	if (!source->getAvailability(leaves->getLeafSize(), file->getSize(), theirs)) return true;

	return file->getChunks().hasFreeRangeWithin(theirs);
}

bool QuickDC::QueueManager::reserveChunk(QueueFile* file, FileSource* source, uint64_t& offset, uint64_t& length)
{
	if (!file) return false;

	/* Without a known size there is nothing to divide into chunks yet. */
	if (!file->getSize()) return false;

	uint64_t want = chunkSizeFor(source);

	/* Round to whole leaves, so each chunk can be checked on its own. */
	LeafHashes* leaves = file->getLeaves();
	if (leaves && leaves->isLoaded())
	{
		uint64_t leaf = leaves->getLeafSize();
		if (leaf)
		{
			want = (want / leaf) * leaf;
			if (!want) want = leaf;
		}
	}

	/* A source that only has part of the file can only be asked for that. */
	ChunkMap theirs;
	if (source && source->isPartial() && leaves && leaves->isLoaded()
		&& source->getAvailability(leaves->getLeafSize(), file->getSize(), theirs))
	{
		if (!file->getChunks().reserveWithin(theirs, want, offset, length)) return false;
	}
	else if (!file->getChunks().reserve(want, offset, length))
	{
		return false;
	}

	if (source) file->setSourceActive(source, true);
	return true;
}

bool QuickDC::QueueManager::verifyChunk(QueueFile* file, uint64_t offset, uint64_t length)
{
	if (!file) return true;

	LeafHashes* leaves = file->getLeaves();
	if (!leaves || !leaves->isLoaded()) return true;   /* nothing to check against */

	Samurai::IO::File part(file->getTempFilename());
	if (!part.open(Samurai::IO::File::Mode::Read))
	{
		QERR("Unable to open '%s' to verify it", file->getTempFilename());
		return false;
	}

	bool ok = leaves->verify(part, offset, length);
	part.close();
	return ok;
}

void QuickDC::QueueManager::commitChunk(QueueFile* file, uint64_t offset, uint64_t length)
{
	if (!file) return;

	file->getChunks().commit(offset, length);
	dirty = true;

	QDBG("Committed %d bytes at offset %d of '%s' (%d/%d done)",
		static_cast<int>(length), static_cast<int>(offset), file->getLocalFilename(),
		static_cast<int>(file->getDoneBytes()), static_cast<int>(file->getSize()));

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

uint64_t QuickDC::QueueManager::chunkSizeFor(const FileSource* source) const
{
	const uint64_t configured = getChunkSize();

	/* Nothing measured yet, or a floor already past where we would grow to. */
	if (!source || configured >= static_cast<uint64_t>(QUICKDC_CHUNK_SIZE_MAX)) return configured;

	const uint64_t rate = source->getThroughput();
	if (!rate) return configured;

	const uint64_t want = rate * QUICKDC_CHUNK_TARGET_SECONDS;

	return std::clamp(want, configured, static_cast<uint64_t>(QUICKDC_CHUNK_SIZE_MAX));
}

uint64_t QuickDC::QueueManager::getChunkSize() const
{
	if (!core || !core->config) return QUICKDC_CHUNK_SIZE;

	core->config->setGroup("Network");
	int configured = core->config->getNumber("Chunk size", QUICKDC_CHUNK_SIZE);
	if (configured <= 0) return QUICKDC_CHUNK_SIZE;

	return static_cast<uint64_t>(configured);
}

size_t QuickDC::QueueManager::getMaxDownloads() const
{
	if (!core || !core->config) return 3;

	core->config->setGroup("Network");
	int configured = core->config->getNumber("Max downloads", 3);
	if (configured <= 0) return 3;

	return static_cast<size_t>(configured);
}

/* ---- file lists ---- */

QuickDC::FilelistRequest* QuickDC::QueueManager::requestFilelist(const char* hub, const char* cid)
{
	FilelistRequest* existing = getFilelistRequest(hub, cid);
	if (existing) return existing;

	/* Whoever asked reads our own list off the disk instead. */
	if (isOurselves(core, hub, cid))
	{
		QDBG("Refusing a file list request: '%s' on '%s' is us", cid ? cid : "",
			hub ? hub : "");
		return 0;
	}

	FilelistRequest* request = new FilelistRequest(hub, cid);
	if (!request->isValid())
	{
		/* The identifier could not name a file; the request has said why. */
		delete request;
		return 0;
	}

	QDBG("Requesting the file list of '%s' on '%s'", cid ? cid : "", hub ? hub : "");

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

	listeners.notify([request](QueueListener& l) { l.EventFilelistFinished(request); });

	delete request;
}

size_t QuickDC::QueueManager::countFilelistRequests() const
{
	return filelists.size();
}

/* ---- token correlation ---- */

/*
 * How many random bytes a token carries.
 *
 * A token is the only secret protecting an inbound peer connection: the CID it
 * is paired with is broadcast in every INF, so anything that can reach the peer
 * port and guess the token gets served. Sixteen bytes is past what can be
 * searched, and short enough that the base32 of it is a reasonable thing to put
 * on a protocol line.
 */
#define QUICKDC_TOKEN_ENTROPY 16

std::string QuickDC::QueueManager::makeToken()
{
	unsigned char raw[QUICKDC_TOKEN_ENTROPY];
	if (!Samurai::Util::secureRandom(raw, sizeof(raw)))
	{
		/*
		 * Nothing weaker is substituted. A token derived from the clock and a
		 * counter is guessable by anyone who knows roughly when this process
		 * started, and a guessable token that looks like a secret is worse than
		 * refusing to make one - the caller can report that it could not ask for
		 * the file, where a weak token silently serves it to whoever asks.
		 */
		QERR("Unable to obtain random bytes for a transfer token");
		return std::string();
	}

	char encoded[Samurai::Util::base32_encode_size(QUICKDC_TOKEN_ENTROPY)];
	Samurai::Util::base32_encode(raw, encoded);
	return std::string(encoded);
}

const char* QuickDC::QueueManager::createRequest(QueueFile* file, FileSource* source)
{
	if (!file || !source) return 0;

	const std::string token = makeToken();
	if (token.empty()) return 0;

	PendingRequest* request = new PendingRequest(token.c_str(), file, source);
	pending[token] = request;

	return request->getToken();
}

const char* QuickDC::QueueManager::createRequest(FilelistRequest* filelist)
{
	if (!filelist) return 0;

	const std::string token = makeToken();
	if (token.empty()) return 0;

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

			/*
			 * What that chunk went at, which is what decides the size of the next
			 * one. Timed from when it was asked for rather than from the first byte,
			 * so the round trip is part of the figure - see chunkSizeFor().
			 */
			if (request->haveReservation())
			{
				source->onChunkDone(request->getReservedLength(),
				                    request->elapsedSeconds());
			}

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
			deadLists[n]->getClientID(), static_cast<int>(deadLists[n]->getRetryCount()));
		removeFilelistRequest(deadLists[n]);
	}

	/* Sources that have failed too often, or that we have not seen in weeks. */
	for (size_t n = 0; n < files.size(); n++)
		files[n]->expireSources();
}

void QuickDC::QueueManager::schedule()
{
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

		/* requestFilelist() turns these away; a queue saved before it did, or a
		   nickname that has since become ours, arrives here instead. */
		if (hub->isLocalUser(list->getClientID())) continue;

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
	for (int prio = static_cast<int>(QueueFile::PrioHigh); prio >= static_cast<int>(QueueFile::PrioLow); prio--)
	{
		for (size_t n = 0; n < files.size() && inFlight < budget; n++)
		{
			QueueFile* file = files[n];
			if (static_cast<int>(file->getPriority()) != prio) continue;
			if (file->isComplete()) continue;
			if (!file->getSize()) continue;

			/* Nothing left to hand out, so a source would find nothing to do. */
			if (!file->getChunks().hasFreeRange()) continue;

			for (size_t s = 0; s < file->countSources() && inFlight < budget; s++)
			{
				FileSource* source = file->getSource(s);
				if (!source || source->isExpired()) continue;
				if (file->isSourceActive(source)) continue;
				if (!source->isReadyToRetry()) continue;
				if (!sourceIsUseful(file, source)) continue;

				QuickDC::Hub* hub = core->hubs->getHubByUrl(source->getHubUrl());
				if (!hub) continue;   /* not logged in there at the moment */

				if (hub->isLocalUser(source->getClientID())) continue;

				const char* created = createRequest(file, source);
				if (!created) continue;

				std::string token(created);
				if (!hub->connectUserByClientID(source->getClientID(), token.c_str()))
				{
					removeRequest(token.c_str());
					continue;
				}

				/* Claim the source now, so we do not ask it twice. */
				source->onAttempt();
				file->setSourceActive(source, true);
				inFlight++;
			}
		}
	}
}

void QuickDC::QueueManager::onFileComplete(QueueFile* file)
{
	if (!file) return;

	if (file->tth.empty())
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

	if (!core || !core->hash)
	{
		QERR("No hash manager available; cannot verify '%s'", file->getLocalFilename());
		return;
	}

	core->hash->addJob(Samurai::IO::File(file->getTempFilename()), verifier);
}

size_t QuickDC::QueueManager::collectSources(::Share::RemoteDir* dir, const char* hub, const char* cid,
                                             size_t depth)
{
	if (!dir || !hub || !cid) return 0;

	/*
	 * The tree came off the wire, and a directory nested inside itself often
	 * enough is a stack overflow rather than a bad file list, so the descent is
	 * bounded here as well as in the parser that built it. Anything below the
	 * bound is left alone: a peer with a share that deep is not one we can
	 * safely walk, and refusing part of it costs us nothing but sources.
	 */
	if (depth >= QUICKDC_FILELIST_MAX_DEPTH)
	{
		QERR("The file list of '%s' nests deeper than %d directories; not descending further",
			cid, static_cast<int>(QUICKDC_FILELIST_MAX_DEPTH));
		return 0;
	}

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
		added += collectSources(dir->getSubdir(n), hub, cid, depth + 1);

	return added;
}

size_t QuickDC::QueueManager::onFilelistDownloaded(FilelistRequest* request, const char* hub, const char* cid)
{
	if (!request || !request->isValid()) return 0;

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
	size_t size = static_cast<size_t>(raw.size());

	if (size > 3 && raw.open(Samurai::IO::File::Mode::Read))
	{
		char magic[3];
		compressed = (raw.read(magic, 3) == 3 && !memcmp(magic, "BZh", 3));
		raw.close();
	}

	/*
	 * What arrived is bounded before any of it is believed. The compressed limit
	 * is the smaller of the two because the decoder has to be fed from it, and
	 * the plain limit applies here too: an uncompressed list is read into memory
	 * whole by the decoder further down.
	 */
	const size_t limit = compressed
		? static_cast<size_t>(QUICKDC_FILELIST_MAX_PACKED)
		: static_cast<size_t>(QUICKDC_FILELIST_MAX_PLAIN);

	if (size > limit)
	{
		QERR("Refusing the %s file list of '%s': %lu bytes, and the limit is %lu",
			compressed ? "compressed" : "plain", request->getClientID(),
			static_cast<unsigned long>(size), static_cast<unsigned long>(limit));
		raw.remove();
		return 0;
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

		if (!raw.open(Samurai::IO::File::Mode::Read))
		{
			QERR("Unable to reopen the downloaded file list");
			return 0;
		}

		Samurai::IO::File target(request->getLocalFilename());
		if (!target.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate))
		{
			QERR("Unable to write the unpacked file list");
			raw.close();
			return 0;
		}

		/*
		 * Both sides are streamed through fixed buffers and the plaintext goes
		 * straight to the file, so neither the download nor what it unpacks to
		 * is ever held in memory whole - and what it unpacks to is what a bomb
		 * makes enormous.
		 */
		Samurai::IO::BZip2Decompressor codec;

		char packed[65536];
		char out[65536];

		std::span<const char> in;
		uint64_t produced = 0;
		bool drained = false;   /* the download has been read to the end */
		const char* problem = 0;

		while (true)
		{
			if (in.empty() && !drained)
			{
				const ssize_t got = raw.read(packed, sizeof(packed));
				if (got < 0)
				{
					problem = "it could not be read";
					break;
				}
				if (!got) drained = true;
				in = std::span<const char>(packed, static_cast<size_t>(got));
			}

			const Samurai::IO::Codec::Progress p = codec.step(in, out);

			if (p.status == Samurai::IO::Codec::Status::Error)
			{
				problem = "it is not valid bzip2 data";
				break;
			}

			produced += p.produced;
			if (produced > static_cast<uint64_t>(QUICKDC_FILELIST_MAX_PLAIN))
			{
				problem = "it unpacks to more than we are willing to read";
				break;
			}

			if (p.produced && target.write(out, p.produced) != static_cast<ssize_t>(p.produced))
			{
				problem = "it could not be written out";
				break;
			}

			in = in.subspan(p.consumed);

			if (p.status == Samurai::IO::Codec::Status::StreamEnd) break;

			/* Nothing moved, and there is nothing left to feed it: the stream
			   stops short of its end. */
			if (!p.consumed && !p.produced)
			{
				problem = "it is truncated";
				break;
			}
		}

		target.close();
		raw.close();

		if (problem)
		{
			/*
			 * Whatever was unpacked is thrown away rather than read: a file list
			 * that is short of entries but looks complete would have us decide a
			 * peer does not hold what we are after.
			 */
			QERR("Discarding the file list of '%s': %s", request->getClientID(), problem);
			target.remove();
			raw.remove();
			return 0;
		}

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
		request->getClientID(), static_cast<int>(added));

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
		/*
		 * Matched on the resolved path, not on the string. A download is named
		 * after [Downloads]/Directory, which is "~/.quickdc/downloads" unless it
		 * is configured otherwise, while the hash manager reports the name
		 * Samurai::IO::File made of it - so the two are only ever equal by
		 * accident.
		 */
		const std::string wanted = Samurai::IO::File::resolvePath(work[n].tempname);

		QueueFile* file = 0;
		for (size_t i = 0; i < files.size(); i++)
		{
			if (Samurai::IO::File::resolvePath(files[i]->getTempFilename()) == wanted)
			{
				file = files[i];
				break;
			}
		}

		if (!file) continue;   /* no longer queued */

		if (!file->tth.empty() && work[n].tth == file->tth)
		{
			QDBG("Download verified: %s", file->getLocalFilename());

			Samurai::IO::File part(file->getTempFilename());
			if (part.rename(std::string(file->getLocalFilename())))
			{
				/*
				 * Announced before the entry is removed: this is the only point
				 * at which a completed download is distinguishable from one that
				 * was cancelled.
				 */
				listeners.notify([file](QueueListener& l) { l.EventQueueFileVerified(file); });

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
			file->getLocalFilename(), file->tth.c_str(), work[n].tth.c_str());

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
	Samurai::IO::File file(path);

	if (!file.exists())
	{
		QDBG("No download queue on disk yet");
		return true;
	}

	size_t size = static_cast<size_t>(file.size());
	if (!size)
	{
		QDBG("The download queue on disk is empty");
		return true;
	}

	if (!file.open(Samurai::IO::File::Mode::Read))
	{
		QERR("Unable to open '%s' for reading", path.c_str());
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

	Samurai::IO::Buffer buffer(static_cast<size_t>(got));
	buffer.append(temp, static_cast<size_t>(got));
	delete[] temp;

	QueueXMLReader handler(this, &buffer);
	bool ret = handler.parse();

	/* Nothing can have been in flight when this was written. */
	for (size_t n = 0; n < files.size(); n++)
		files[n]->getChunks().clearReservations();

	dirty = false;
	lastSaved.reset();

	QDBG("Loaded %d file(s) into the download queue", static_cast<int>(files.size()));
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
		xmlout->append(std::to_string(file->getSize()));
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
			xmlout->append(std::to_string(static_cast<uint64_t>(source->getLastSeen().getInternalData())));
			xmlout->append("\" retries=\"");
			xmlout->append(std::to_string(static_cast<uint64_t>(source->getRetryCount())));
			xmlout->append("\" />" XML_NEWLINE);

			free(url);
		}

		uint64_t start = 0;
		uint64_t end = 0;
		for (size_t c = 0; file->getChunks().getRange(c, start, end); c++)
		{
			xmlout->append(XML_INDENT XML_INDENT "<Chunk start=\"");
			xmlout->append(std::to_string(start));
			xmlout->append("\" end=\"");
			xmlout->append(std::to_string(end));
			xmlout->append("\" />" XML_NEWLINE);
		}

		xmlout->append(XML_INDENT "</File>" XML_NEWLINE);
		stored++;
	}

	xmlout->append("</DownloadQueue>" XML_NEWLINE);

	/* Write beside the real file, then move it into place. */
	bool ret = false;
	Samurai::IO::File out(pathNew);

	if (out.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate))
	{
		size_t length = xmlout->size();
		char* data = new char[length];
		xmlout->pop(data, length);

		ssize_t written = out.write(data, length);
		out.close();
		delete[] data;

		if (written == static_cast<ssize_t>(length))
		{
			Samurai::IO::File target(path);
			target.remove();

			ret = out.rename(path);
			if (!ret)
				QERR("Unable to move '%s' into place", pathNew.c_str());
		}
		else
		{
			QERR("Unable to write the download queue");
		}
	}
	else
	{
		QERR("Unable to open '%s' for writing", pathNew.c_str());
	}

	delete xmlout;

	if (ret)
	{
		QDBG("Saved %d file(s) to the download queue", static_cast<int>(stored));
		(void) stored;
		dirty = false;
		lastSaved.reset();
	}

	return ret;
}
