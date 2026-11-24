/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef MINGW
#define SEARCH_DEBUG
#endif

#include "quickdc.h"
#include <algorithm>
#include <stdio.h>
#include <span>
#include <string>
#include <string_view>
#include <string.h>
#include "sharemanager.h"
#include "sharedir.h"
#include "sharefile.h"
#include "config/preferences.h"
#include <samurai/io/file.h>
#include <samurai/stdc.h>
#include <samurai/io/compression.h>
#include <samurai/io/buffer.h>
#include "api/attachments.h"
#include "api/core.h"
#include "hash/hashcache.h"
#include "hash/hashjob.h"
#include "hash/hashmanager.h"
#include "network/adc/cid.h"
#include "network/transfer.h"
#include "network/transfermanager.h"
#include "config/settings.h"

/* An upper bound on "Total shares", which is read from the configuration file. */
#define MAX_SHARES 1024

#ifdef SEARCH_DEBUG
#include <sys/time.h>
#include <time.h>
#endif

#define BUFSIZE 65536

#define DATADUMP

QuickDC::Share::Manager::Manager(QuickDC::Preferences* config_, QuickDC::Hash::Manager* mgr,
                                 const char* dir)
	: size(0), files(0), config(config_), hash(mgr),
	  listdir(dir ? dir : "~/.quickdc"), filelists(false), attaching(false) {
	/* Every TTH root is known once hashing falls idle, so that is when the lists
	   are worth building and when an attachment that never got one is given up. */
	if (hash) hash->setIdleCallback([this] {
		createFileLists(false);
		if (onHashed) onHashed();
	});
}

QuickDC::Share::Manager::Attachment::~Attachment() {
	/* The directory exists to hold this one file, and deletes it. */
	delete dir;
}

void QuickDC::Share::Manager::initialize() {
	QDBG("QuickDC::Share::Manager::initialize()");
	
	config->setGroup("Shares");

	/*
	 * Clamped, because the count comes from a file: shares are directories
	 * someone chose by hand, so anything past this is a typo or a corrupt
	 * configuration, and a negative value walked the loop nowhere useful.
	 */
	const int configured = QuickDC::Settings::read(*config, QuickDC::Settings::TotalShares);
	const int num = std::clamp(configured, 0, MAX_SHARES);

	if (num != configured)
		QERR("'Total shares' is %d, which is not a number of shares; using %d", configured, num);

	QDBG("Shares found: %d", num);

	/**
	 * We store the directories shared in the following format:
	 * "fakename:/real/path/name"
	 */
	for (int i = 0; i < num; i++) {
		/* Wide enough for the longest key the loop above can reach, and
		   snprintf so that a wider one truncates rather than overflows. */
		char key[32];
		snprintf(key, sizeof(key), "Share %d", i);
		char* data = strdup(config->peekString("Shares", key, "").c_str());
		
		QDBG("Share %d='%s' (%s)", i, data, key);
		
		if (strlen(data) == 0) {
			config->remove(key);
		} else {
			char* split = strchr(data, ':');
			if (split && strlen(split) > 0) {
				split[0] = '\0';
				char* fakepath = data;
				char* realpath = &split[1];
				QuickDC::Share::Dir* dir = new QuickDC::Share::Dir(realpath, fakepath, this);
				this->addShare(dir);
			} else {
				config->remove(key);
			}
		}
		free(data);
	}

	loadAttachments();

	createFileLists(true);
}

QuickDC::Share::Manager::~Manager() {
	if (hash) hash->setIdleCallback(nullptr);

	/* delete all shares here, they will remove themselves */
	while (dirs.size()) {
		QuickDC::Share::Dir* dir = dirs.back();
		dirs.pop_back();
		delete dir;
	}

	/* The attachments belong to no share, so nothing else would free them:
	   'attachments' does it, each record taking its directory and its file. */
	attachments.clear();
}

void QuickDC::Share::Manager::addSize(uint64_t sz) {
	/* Not while attaching: an attachment's Share::File reports its size through
	   its directory like any other, but it is not part of the share and a hub
	   must be told the same size as before. */
	if (attaching) return;

	size += sz;
	files++;
}

void QuickDC::Share::Manager::addShare(QuickDC::Share::Dir* dir) {
	dirs.push_back(dir);
}

void QuickDC::Share::Manager::removeShare(QuickDC::Share::Dir* dir) {
	dir->manager = 0;
}

/* A public name is a peer's request, so it is compared rather than trusted, and
   no name at all names nothing this share can serve. */
Samurai::IO::File* QuickDC::Share::Manager::getFile(const char* pubname) const {
	if (!pubname) return 0;

#ifdef DATADUMP
	QDBG("QuickDC::Manager::getFile(): '%s'", pubname);
#endif

	const std::string_view name(pubname);

	if (isFileList(name)) {

		if (!filelists)
		{
			QERR("Filelists not generated yet");
			return 0;
		}
		else
		{
			/* The same directory createFileLists() wrote them to. */
			Samurai::IO::File* file =
				new Samurai::IO::File(listdir + "/" + pubname);

#ifdef DATADUMP
			QDBG("xxx Looking up file: %s (%s)", file->getName().c_str(), pubname);
#endif
			if (!file->exists())
			{
				delete file;
				return 0;
			}
			return file;
		}
	} else if (Samurai::Util::istarts_with(name, "TTHL/")) {
		/* Leaf hashes, which are not a file in the share at all: they live
		   inside the hash cache, and getLeafData() is what serves them. Any
		   other name is a file the walk below has to look for. */
		return 0;
	} else {
		/*
		 * The walk cuts the name into components in place, so it needs a copy it
		 * may write to - held by value, because every exit below used to leave
		 * the strdup()'d one behind, and a peer chooses both the name and how
		 * many times it asks.
		 */
		std::string path(pubname);
		char* iter = path.data();
		char* npos = 0;

		/* A public name is rooted, and a peer asks for the name it was given.
		   The first component is the share's own name, not the empty one in
		   front of a leading separator. */
		if (*iter == '/') iter++;

		npos = strchr(iter, '/');
		if (!npos) return 0; // Must have at least one / unless file is a file list
		npos[0] = 0;

		QuickDC::Share::Dir* root = 0;
		for (size_t i = 0; i < dirs.size(); i++) {
			if (dirs[i]->getBaseName() == iter) {
				iter = &npos[1];
				npos = strchr(iter, '/');
				root = dirs[i];
				break;
			}
		}

		QuickDC::Share::Dir* dir = root;
		while (dir) {
			QDBG("*** lookup: str='%s'", iter);
			if (npos) {
				/* Cut here as well, the way the first component is cut above:
				   getDir() compares against a directory's base name, and what
				   is left of the path is not one. */
				npos[0] = 0;
				dir = dir->getDir(iter);
				iter = &npos[1];
				npos = strchr(iter, '/');
			} else {
				QDBG("*** looking for file in dir='%s', file='%s'", dir->getPath().c_str(), iter);
				QuickDC::Share::File* shf = dir->getFile(iter);
				if (shf)
				{
					QDBG("Found shared file");
					return const_cast<Samurai::IO::File*>(&shf->getFile());
				} else {
					QDBG("Did not find this shared file");
				}
				return 0;
			}
		}
	}
	
#ifdef DATADUMP
	QDBG("Not found");
#endif
	return 0;
	
}

bool QuickDC::Share::Manager::isFileList(std::string_view name) {
	return name == "MyList.DcLst" || name == "MyList.bz2" || name == "files.xml" || name == "files.xml.bz2";
}

QuickDC::Share::File* QuickDC::Share::Manager::findByTTH(const char* tth) const {
	std::map<std::string, QuickDC::Share::File*>::const_iterator it = tthTable.find(std::string(tth));
	if (it != tthTable.end())
		return (*it).second;

	/* An attachment is in tthTable as well, but its record is what owns it, so
	   asking here too means anything that empties the table and fills it in
	   again from the shared directories cannot lose an attachment. */
	for (const std::unique_ptr<Attachment>& record : attachments)
		if (record->file && record->tth == tth)
			return record->file;

	return 0;
}

QuickDC::Share::File* QuickDC::Share::Manager::findInDirByTTH(QuickDC::Share::Dir* dir, const std::string& tth) {
	for (QuickDC::Share::File* file : dir->files) {
		const char* sum = file->getTTH();
		if (sum && tth == sum)
			return file;
	}

	for (QuickDC::Share::Dir* sub : dir->subdirs)
		if (QuickDC::Share::File* found = findInDirByTTH(sub, tth))
			return found;

	return 0;
}

/* The shared directories only, so what is found is a file that is reachable by
   its hash for being shared rather than for having been attached. */
QuickDC::Share::File* QuickDC::Share::Manager::findSharedByTTH(const std::string& tth) const {
	if (tth.empty()) return 0;

	for (QuickDC::Share::Dir* dir : dirs)
		if (QuickDC::Share::File* found = findInDirByTTH(dir, tth))
			return found;

	return 0;
}

Samurai::IO::File* QuickDC::Share::Manager::getFileByTTH(const char* tth) {
	QuickDC::Share::File* shf = findByTTH(tth);
	if (!shf) return 0;
	return const_cast<Samurai::IO::File*>(&shf->getFile());
}


#if defined(BZIP2) || defined(ZLIB)
static bool process(Samurai::IO::Codec* codec, Samurai::IO::Buffer* input, Samurai::IO::Buffer* output) {
	const std::string source = input->copyRange(0, input->size());
	std::span<const char> in(source);
	char outbuf[BUFSIZE];

	/*
	 * Driven off the status, not off whether anything moved. Stopping as soon
	 * as nothing was consumed ends the loop on the codec's first flush call,
	 * and a flush too large for outbuf needs several - which truncated the
	 * compressed file list, leaving a file no client could unpack.
	 */
	while (true) {
		const Samurai::IO::Codec::Progress p = codec->step(in, outbuf);
		if (p.status == Samurai::IO::Codec::Status::Error) return false;

		output->append(outbuf, p.produced);
		in = in.subspan(p.consumed);

		if (p.status == Samurai::IO::Codec::Status::StreamEnd) break;
		if (!p.consumed && !p.produced) break;
	}
	return true;
}
#endif

void QuickDC::Share::Manager::createFileLists(bool incomplete) {
	QDBG("QuickDC::Share::Manager::createFileLists");

	/* The hash manager calls this when the last job is collected, which is the
	   only moment an attachment that will never be hashed can be recognised. */
	sweepAttachments();

	Samurai::IO::Buffer* xmlout = new Samurai::IO::Buffer();
	
	createFileList(xmlout, 0, incomplete);
	
	{
#ifdef DATADUMP
		QDBG("XML file list will be %i bytes long", static_cast<int>(xmlout->size()));
#endif
		Samurai::IO::File xmllist(listdir + "/files.xml");
		if (xmllist.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate)) {
			char* out = new char[xmlout->size()];
			xmlout->pop(out, xmlout->size());
			int written = xmllist.write(out, xmlout->size());
			if (written == -1) QDBG("Error writing files.xml");
			xmllist.close();
			delete[] out;
		}
	}

#ifdef BZIP2
	{
		Samurai::IO::File dclist(listdir + "/files.xml.bz2");
		Samurai::IO::BZip2Compressor* filter = new Samurai::IO::BZip2Compressor();
		Samurai::IO::Buffer* writer = new Samurai::IO::Buffer();
		bool ok = process(filter, xmlout, writer);
		if (ok && dclist.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate)) {
			int written = dclist.write(writer, writer->size());
			if (written == -1) QDBG("Error writing files.xml.bz2");
			dclist.close();
		}
		delete filter;
		delete writer;
	}
#endif // BZIP2
#ifdef DATADUMP
	QDBG("Lists written to disk");
#endif
	
	delete xmlout;
	
	filelists = true;
}

#define NEWLINE "\r\n"
#define INDENT '\x9'
#define SEPARATOR '|'

#define XML_NEWLINE "\r\n"
#define XML_INDENT "\x9"

namespace XML {
	extern void escape(Samurai::IO::Buffer* buffer, const char* str);
}

/**
 * FIXME: Should replace any unsafe XML characters
 *        and convert to utf-8.
 */
void QuickDC::Share::Manager::createFileList(Samurai::IO::Buffer* xmlout, QuickDC::Share::Dir* dir, bool incomplete) {
	if (!dir)
	{
		xmlout->append("<?xml version=\"1.1\" encoding=\"utf-8\" standalone=\"yes\"?>" XML_NEWLINE);
		xmlout->append("<FileListing Version=\"1\" CID=\"");
		xmlout->append(QuickDC::Settings::read(*config, QuickDC::Settings::ClientId).c_str());
		xmlout->append("\" Generator=\"" PRODUCT "/" VERSION " (" SYSTEM "/" CPU ")\" Base=\"/\">" XML_NEWLINE);
		
		for (size_t i = 0; i < dirs.size(); i++) {
			QuickDC::Share::Dir* dir = dirs[i];
			
			xmlout->append(XML_INDENT "<Directory Name=\"");
			XML::escape(xmlout, dir->getBaseName().c_str());
			xmlout->append("\"");
			if (incomplete) xmlout->append(" Incomplete=\"1\"");
			xmlout->append(">" XML_NEWLINE);
			createFileList(xmlout, dir, incomplete);
			xmlout->append(XML_INDENT "</Directory>" XML_NEWLINE);
		}
		xmlout->append("</FileListing>" XML_NEWLINE);
		
	} else {
		
		// Add files to lists
		for (size_t i = 0; i < dir->files.size(); i++) {
			QuickDC::Share::File* file = dir->files[i];
			
			for (int tab = 0; tab < file->getLevel()+1; tab++) xmlout->append(XML_INDENT);

			xmlout->append("<File Name=\"");
			XML::escape(xmlout, file->getBaseFileName().c_str());
			xmlout->append("\" Size=\"");
			xmlout->append(file->getSize());
			xmlout->append("\" TTH=\"");
			if (file->getTTH())	xmlout->append(file->getTTH());
			xmlout->append("\" />" XML_NEWLINE);
		}
		
		// Add sub-directories
		for (size_t i = 0; i < dir->subdirs.size(); i++) {
			QuickDC::Share::Dir* mdir = dir->subdirs[i];

			for (int tab = 0; tab < mdir->getLevel()+1; tab++) xmlout->append(XML_INDENT);

			xmlout->append("<Directory Name=\"");
			XML::escape(xmlout, mdir->getBaseName().c_str());
			xmlout->append("\"");
			if (incomplete) xmlout->append(" Incomplete=\"1\"");
			xmlout->append(">" XML_NEWLINE);
			
			createFileList(xmlout, mdir, incomplete);
			
			for (int tab = 0; tab < mdir->getLevel()+1; tab++) xmlout->append(XML_INDENT);
			xmlout->append("</Directory>" XML_NEWLINE);
		}
	}
}

void QuickDC::Share::Manager::addLookupFile(const char* name, const char* tth, QuickDC::Share::File* ptr) {
	/*
	 * An attachment reaches this the same way a shared file does, since it is an
	 * ordinary Share::File, but it is left out of the name index: a file that is
	 * offered by its hash alone must not answer a search for its name.
	 */
	if (!attaching && !isAttachment(ptr))
		searchTable.push_back( std::pair<std::string, QuickDC::Share::File*>(std::string(name), ptr) );

	tthTable.insert( std::pair<std::string, QuickDC::Share::File*>(std::string(tth), ptr) );
}

bool QuickDC::Share::Manager::attach(const char* path, QuickDC::Share::AttachmentListener* listener,
                                     QuickDC::Share::Manager::Persistence persistence,
                                     QuickDC::Share::Manager::SlotCost slot) {
	if (!path || !path[0] || !listener) return false;

	Samurai::IO::File file(path);
	if (!file.isRegular()) return false;
	if (!file.open(Samurai::IO::File::Mode::Read)) return false;
	file.close();

	QDBG("QuickDC::Share::Manager::attach('%s')", path);

	QuickDC::Share::Manager::Attachment* known = findAttachment(file.getName());
	if (known) {
		/* Promoted, never demoted: whoever asked for the file to be remembered was
		   promised a URI that outlives the session, and a later caller who only
		   needed it for this one cannot take that back. */
		if (persistence == Persistence::Remembered && known->persistence == Persistence::Session) {
			known->persistence = Persistence::Remembered;
			if (!known->tth.empty()) saveAttachments();
		}

		/* And slot-free in the same direction, for the same reason: a caller that
		   needs the file served whatever else is running cannot have that taken back
		   by one that did not. The bytes are the same either way, one hash being one
		   content, so nothing larger becomes exempt by this. */
		if (slot == SlotCost::Free) known->slot = SlotCost::Free;

		if (known->tth.empty())
			known->listeners.push_back(listener);
		else
			listener->EventAttachmentReady(path, known->tth.c_str(), known->size);
		return true;
	}

	if (!hash) {
		listener->EventAttachmentFailed(path, "There is no hash manager to hash the file with");
		return true;
	}

	std::unique_ptr<Attachment> record = std::make_unique<Attachment>();
	record->path = path;
	record->name = file.getName();
	record->size = static_cast<uint64_t>(file.size());
	record->persistence = persistence;
	record->slot = slot;
	record->listeners.push_back(listener);
	attachments.push_back(std::move(record));

	/*
	 * The same mechanism a shared file is hashed by, so the hash cache applies:
	 * a file that has been hashed before costs nothing and is answered from
	 * inside this call, and anything else is hashed on the hash worker thread
	 * and reported from Hash::Manager::process().
	 */
	hash->addJob(file, this);
	return true;
}

void QuickDC::Share::Manager::forgetListener(QuickDC::Share::AttachmentListener* listener) {
	if (!listener) return;

	/*
	 * Every record, because one caller may have several files in flight, and the
	 * record itself is left exactly as it is: what it is waiting for is the hash,
	 * which has nothing to do with who was going to be told.
	 */
	for (std::unique_ptr<Attachment>& record : attachments) {
		std::vector<AttachmentListener*>& waiting = record->listeners;

		waiting.erase(std::remove(waiting.begin(), waiting.end(), listener),
		              waiting.end());
	}
}

bool QuickDC::Share::Manager::detach(const std::string& tth) {
	/* A record waiting for its hash has an empty one, and is not something a
	   caller can name: it has never been offered under any hash. */
	if (tth.empty()) return false;

	/*
	 * Collected first, because forgetting a record destroys it. Every one of them
	 * goes: the same content can be attached under several names, of which one
	 * holds the hash table entry and the rest only describe the same file, and a
	 * record left behind would put the hash back in the configuration and offer it
	 * again at the next start-up.
	 */
	std::vector<Attachment*> matched;
	for (const std::unique_ptr<Attachment>& record : attachments)
		if (record->tth == tth)
			matched.push_back(record.get());

	if (matched.empty()) return false;

	QDBG("QuickDC::Share::Manager::detach('%s')", tth.c_str());

	bool remembered = false;
	for (Attachment* record : matched) {
		if (record->persistence == Persistence::Remembered) remembered = true;
		forgetAttachment(*record);
	}

	/* Only if the configuration had anything to say about it, so that taking back
	   a session attachment does not rewrite the file. */
	if (remembered) saveAttachments();

	return true;
}

QuickDC::Share::Manager::Attachment* QuickDC::Share::Manager::findAttachment(const std::string& name) const {
	for (const std::unique_ptr<Attachment>& record : attachments)
		if (record->name == name)
			return record.get();

	return 0;
}

bool QuickDC::Share::Manager::isAttachment(const QuickDC::Share::File* ptr) const {
	for (const std::unique_ptr<Attachment>& record : attachments)
		if (record->file == ptr)
			return true;

	return false;
}

bool QuickDC::Share::Manager::isSlotFreeByTTH(const char* tth) const {
	if (!tth || !tth[0]) return false;

	for (const std::unique_ptr<Attachment>& record : attachments)
		if (record->slot == SlotCost::Free && record->tth == tth)
			return true;

	return false;
}

bool QuickDC::Share::Manager::isSlotFreeHandle(const Samurai::IO::File* handle) const {
	if (!handle) return false;

	for (const std::unique_ptr<Attachment>& record : attachments) {
		if (record->slot != SlotCost::Free || record->tth.empty()) continue;

		const QuickDC::Share::File* served = findByTTH(record->tth.c_str());
		if (served && &served->getFile() == handle) return true;
	}

	return false;
}

size_t QuickDC::Share::Manager::countSlotFreeUploads(QuickDC::TransferManager* transfers) const {
	if (!transfers) return 0;

	size_t count = 0;

	/*
	 * The manager's own iterator, which is the only way to walk it, so this cannot
	 * run inside another walk of the same list. Nothing does: the transfers are
	 * driven by their sockets, and the places that enumerate them - this and
	 * countUploads() - are called one after the other while a request is answered.
	 */
	for (QuickDC::Transfer* xfer = transfers->first(); xfer; xfer = transfers->next()) {
		if (!xfer->isUpload()) continue;
		if (isSlotFreeHandle(xfer->getFile())) count++;
	}

	return count;
}

bool QuickDC::Share::Manager::mayServeWithoutSlot(const char* tth, QuickDC::TransferManager* transfers) const {
	if (!isSlotFreeByTTH(tth)) return false;

	const int bound = (config) ? config->peekNumber("Network", "Slot free uploads",
		DEFAULT_SLOT_FREE_UPLOADS) : DEFAULT_SLOT_FREE_UPLOADS;

	/*
	 * Counted rather than assumed to be plentiful: a post is bounded in size by the
	 * hub that took it, but the number of connections a peer opens is bounded by
	 * nothing we control, and serving without a slot means the ordinary refusal is
	 * not there to stop them.
	 */
	if (bound <= 0) return false;

	return countSlotFreeUploads(transfers) < static_cast<size_t>(bound);
}

bool QuickDC::Share::Manager::registerAttachment(QuickDC::Share::Manager::Attachment& record) {
	if (tthTable.count(record.tth)) {
		/* Reachable already: the very same file is shared, or was attached
		   under another name. Nothing to add, and nothing to own. */
		QDBG("Attachment '%s' is already reachable by TTH", record.path.c_str());
		return true;
	}

	/*
	 * A Share::File expects a directory: it reports its size through it, and
	 * finds the manager and the hash cache through it. This one is not among
	 * 'dirs', which is what keeps the file out of every file list, and scanning
	 * it finds nothing because the path it is given is the file itself.
	 */
	attaching = true;
	record.dir = new QuickDC::Share::Dir(record.name, "attachments", this);
	record.file = new QuickDC::Share::File(record.name, record.dir);
	record.dir->files.push_back(record.file);
	attaching = false;

	tthTable.insert( std::pair<std::string, QuickDC::Share::File*>(record.tth, record.file) );
	return true;
}

void QuickDC::Share::Manager::forgetAttachment(QuickDC::Share::Manager::Attachment& record) {
	/*
	 * Only the entry this record put there. registerAttachment() adds nothing and
	 * owns nothing when the hash was reachable already - the same content is
	 * shared, or was attached under another name - and erasing that entry would
	 * stop answering for a file there is still every reason to serve.
	 */
	if (record.file) {
		std::map<std::string, QuickDC::Share::File*>::iterator it = tthTable.find(record.tth);
		if (it != tthTable.end() && (*it).second == record.file) {
			tthTable.erase(it);

			/* Handed over rather than dropped when the share holds the same
			   content: the shared file's own insertion found this entry already
			   there and did nothing, so nothing else will put it back. */
			if (QuickDC::Share::File* shared = findSharedByTTH(record.tth))
				tthTable.insert( std::pair<std::string, QuickDC::Share::File*>(record.tth, shared) );
		}
	}

	/*
	 * The hashing job the attachment's own Share::File registered as it was built,
	 * in the rare case that it is still waiting. ~Share::File cannot take that
	 * back: it reaches the hash manager through its directory, and the directory
	 * clears its manager pointer before deleting the files. A job left holding a
	 * listener that has been freed calls into it on the next turn of the loop.
	 */
	if (record.file && !record.file->getTTH() && hash)
		hash->removeJob(record.file->getFile());

	for (std::vector< std::unique_ptr<Attachment> >::iterator it = attachments.begin(); it != attachments.end(); it++) {
		if ((*it).get() == &record) {
			/* The record owns the directory, which owns the file. */
			attachments.erase(it);
			break;
		}
	}
}

void QuickDC::Share::Manager::dropAttachment(QuickDC::Share::Manager::Attachment& record, const char* reason) {
	const std::string path = record.path;
	std::vector<QuickDC::Share::AttachmentListener*> waiting;
	waiting.swap(record.listeners);

	/* Forgotten before anyone is told, so a listener that responds by attaching
	   the same file again starts over rather than waiting for a hash that is
	   not coming. */
	forgetAttachment(record);

	for (QuickDC::Share::AttachmentListener* listener : waiting)
		listener->EventAttachmentFailed(path.c_str(), reason);
}

void QuickDC::Share::Manager::sweepAttachments() {
	if (!hash || hash->isBusy()) return;

	/*
	 * A hash job that fails is only written to the log - the hash manager tells
	 * no listener - so a failure is recognised by its absence instead: with no
	 * hashing left to do, an attachment without a hash is never getting one.
	 */
	std::vector<Attachment*> failed;
	for (const std::unique_ptr<Attachment>& record : attachments)
		if (record->tth.empty())
			failed.push_back(record.get());

	for (Attachment* record : failed)
		dropAttachment(*record, "The file could not be hashed");
}

/**
 * A base32 tiger root is always this long, which is what makes the path in an
 * [Attachments] entry findable by offset - and so allowed to contain spaces.
 */
#define TTH_BASE32_LENGTH 39

void QuickDC::Share::Manager::saveAttachments() {
	config->setGroup("Attachments");

	const int previous = QuickDC::Settings::read(*config, QuickDC::Settings::TotalAttachments);
	int total = 0;

	for (const std::unique_ptr<Attachment>& record : attachments) {
		if (record->tth.empty()) continue;
		/* A session attachment is offered for as long as the caller keeps it and
		   not one moment longer, so nothing about it is written down. */
		if (record->persistence != Persistence::Remembered) continue;

		config->setString("File " + std::to_string(total), record->tth + " " + record->name);
		total++;
	}

	/* Whatever a longer list left behind, so nothing outlives the Total. */
	for (int i = total; i < previous; i++) {
		config->remove("File " + std::to_string(i));
	}

	config->setNumber("Total", total);

	/* Now rather than at shutdown: an attachment that has been handed out in a
	   message must survive a crash. */
	config->write();
}

void QuickDC::Share::Manager::loadAttachments() {
	config->setGroup("Attachments");
	const int total = QuickDC::Settings::read(*config, QuickDC::Settings::TotalAttachments);
	if (total <= 0) return;

	QDBG("Attachments remembered: %d", total);

	if (!hash) {
		/* Nothing can be offered without one, and that says nothing about the
		   entries themselves, so they are left in the configuration rather than
		   rewritten as if they were stale. */
		QERR("Cannot offer %d remembered attachment(s) without a hash manager", total);
		return;
	}

	bool forgotten = false;
	for (int i = 0; i < total; i++) {
		char key[16];
		snprintf(key, sizeof(key), "File %d", i);

		/* Copied out: the pointer belongs to the setting, which saveAttachments()
		   may overwrite. */
		const std::string entry = config->peekString("Attachments", key, "");

		if (entry.size() < TTH_BASE32_LENGTH + 2 || entry[TTH_BASE32_LENGTH] != ' ') {
			QERR("Attachment %d is not a hash and a path: '%s'", i, entry.c_str());
			forgotten = true;
			continue;
		}

		const std::string tth = entry.substr(0, TTH_BASE32_LENGTH);
		const std::string path = entry.substr(TTH_BASE32_LENGTH + 1);

		if (!adoptAttachment(tth, path)) {
			QDBG("Forgetting attachment '%s'", path.c_str());
			forgotten = true;
		}
	}

	if (forgotten) saveAttachments();
}

bool QuickDC::Share::Manager::adoptAttachment(const std::string& tth, const std::string& path) {
	Samurai::IO::File file(path);
	if (!file.exists() || !file.isRegular()) return false;

	if (findAttachment(file.getName())) return true;

	/*
	 * Not offered on trust: the hash cache is asked whether this is still the
	 * file the hash was taken from. An entry only survives the cache being read
	 * if the file's size and modification date are unchanged, and the root
	 * derived from the leaves it holds says whether that file is the one the
	 * remembered hash belongs to. Neither reads the file itself.
	 */
	QuickDC::Hash::CacheEntry* cached = 0;
	if (!hash->lookup(file, cached) || !cached) return false;
	if (cached->getAlgorithm() != QuickDC::Hash::HashTigerTree) return false;

	QuickDC::Hash::SimpleJob known(hash, this, cached);
	const char* sum = known.getHashSum();
	if (!sum || tth != sum) return false;

	std::unique_ptr<Attachment> record = std::make_unique<Attachment>();
	record->path = path;
	record->name = file.getName();
	record->tth = tth;
	record->size = static_cast<uint64_t>(file.size());
	attachments.push_back(std::move(record));

	/* The same registration attach() ends in, so the two cannot drift apart. */
	QuickDC::Share::Manager::Attachment& adopted = *attachments.back();
	if (!registerAttachment(adopted)) {
		dropAttachment(adopted, "There is no core for the file to be offered from");
		return false;
	}

	QDBG("Offering remembered attachment '%s'", path.c_str());
	return true;
}

void QuickDC::Share::Manager::EventFileStarted(const QuickDC::Hash::Job*) { }
void QuickDC::Share::Manager::EventFileQueued(const QuickDC::Hash::Job*) { }
void QuickDC::Share::Manager::EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t, uint64_t) { }

void QuickDC::Share::Manager::EventFileHashed(const QuickDC::Hash::Job* job) {
	QuickDC::Share::Manager::Attachment* record = findAttachment(job->getFile().getName());
	if (!record) return;

	const char* sum = job->getHashSum();
	if (!sum || !sum[0]) {
		dropAttachment(*record, "The file could not be hashed");
		return;
	}

	record->tth = sum;
	if (!registerAttachment(*record)) {
		dropAttachment(*record, "There is no core for the file to be offered from");
		return;
	}

	/* Before anyone is told, so the hash we are about to hand out is remembered
	   even if the client never gets a chance to shut down cleanly. */
	if (record->persistence == Persistence::Remembered)
		saveAttachments();

	std::vector<QuickDC::Share::AttachmentListener*> waiting;
	waiting.swap(record->listeners);

	for (QuickDC::Share::AttachmentListener* listener : waiting)
		listener->EventAttachmentReady(record->path.c_str(), record->tth.c_str(), record->size);
}

void QuickDC::Share::Manager::search(SearchRequest* req) {
	if (!req) return;
	
#ifdef SEARCH_DEBUG
	struct timeval before;
	gettimeofday(&before, 0);
#endif
	
	int max = req->getMaxResults();
	uint64_t size = req->getSize();
	enum Share::SearchRequest::SizePolicy size_type = req->getSizePolicy();
	int hits = 0;
	
	if (req->isTTH()) {

		/* Search TTH */
		QSEARCH("tth search for: %s", req->getTTH());
		QuickDC::Share::File* found = findByTTH(req->getTTH());
		if (found) {
			hits++;
			SearchReply reply(req, found);
			QSEARCH("(tth hit): %s", found->getPublicName().c_str());
			if (req->event_handler)
				req->event_handler->EventSearchReply(&reply);
		}
		
	} else {
		if (req->getInclusions().empty()) return; /* Invalid search */

		for (size_t n = 0; n < searchTable.size(); n++) {
			std::pair<std::string, QuickDC::Share::File*>& current = searchTable[n];
			const std::string& line = current.first;

			/* Every term has to be there, in whatever order they were given.
			   Searching on from where the last one matched made the order part
			   of the question, which it is not. */
			bool matches = true;
			for (const std::string& term : req->getInclusions()) {
				if (!Samurai::Util::icontains(line, term)) { matches = false; break; }
			}
			if (!matches) continue;

			/* And none of the excluded ones. Keeping the file when the term was
			   found is backwards: it answered with the one thing the asker had
			   said they did not want, and hid everything else. */
			for (const std::string& term : req->getExclusions()) {
				if (Samurai::Util::icontains(line, term)) { matches = false; break; }
			}
			if (!matches) continue;

			/* match extension */
			if (!req->getExtensions().empty()) {
				bool extensionOK = false;
				for (const std::string& ext : req->getExtensions()) {
					if (current.second->getFile().matchExtension(ext.c_str())) {
						extensionOK = true;
						break;
					}
				}
				if (!extensionOK) continue;
			}
			
			/* match size */
			if (current.second->match(size, size_type)) {
				current.second->hit();
				hits++;
				SearchReply reply(req, current.second);
				QSEARCH("(hit)   : %s", current.second->getPublicName().c_str());
				if (req->event_handler)
					req->event_handler->EventSearchReply(&reply);
			}
			
			
			// Return when reaching max results limit
			if (hits >= max) break;
		}
	}
	
#ifdef SEARCH_DEBUG
	struct timeval after;
	gettimeofday(&after, 0);
	long ms = (((after.tv_usec - before.tv_usec) / 1000) + ((after.tv_sec - before.tv_sec) * 1000));
	QSEARCH("Search '%p', returned %d hits in %d ms", req, hits, static_cast<int>(ms));
	(void) ms;
#endif
}


