/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "network/adc/client/adcclientsession.h"
#include "network/adc/client/adchubsession.h"

#include "network/usermanager.h"
#include "config/preferences.h"
#include "api/hub.h"
#include "api/core.h"
#include "share/sharemanager.h"
#include "hash/hashmanager.h"
#include "hash/hashcache.h"
#include "network/connectionmanager.h"
#include "network/hubmanager.h"
#include "network/securitymanager.h"
#include "network/transfer.h"
#include "network/transfermanager.h"
#include "network/downloadqueue.h"
#include "network/filesource.h"
#include "network/leafhashes.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/adccommands.h"
#include "network/adc/parser.h"
#include "network/adc/cid.h"
#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <string>

/* One field: severity digit followed by a two digit code. */
static std::string adc_status_code(int severity, int code)
{
	char buf[8];
	snprintf(buf, sizeof(buf), "%d%02d", severity, code);
	return std::string(buf);
}

ADC::ClientSession::ClientSession(Samurai::IO::Net::URL* url, ADC::HubSession* hubsession_) :
	QuickDC::Connection(url, "ADC client"),
	hubsession(hubsession_),
	isClient(true),
	secure(false)
{
	/* The destructor free()s all of these, so they cannot be left unset. */
	token = 0;
	remote_sid = 0;
	remote_cid = 0;

	initialize();
	proxy = new ADC::CommandProxy(socket, this);
	QDBG("ADC::ClientSession()");
}

ADC::ClientSession::ClientSession(Samurai::IO::Net::Socket* sock, ADC::HubSession* hubsession_, const char* token_, const char* sid_, bool secure_) :
	QuickDC::Connection(sock, secure_ ? "ADCS client" : "ADC client"),
	hubsession(hubsession_),
	isClient(false),
	secure(secure_)
{
	if (token_) token = strdup(token_);
	else token = 0;
	
	if (sid_) remote_sid = strdup(sid_);
	else remote_sid = 0;

	remote_cid = 0;

	initialize();
	proxy = new ADC::CommandProxy(socket, this);
	QDBG("ADC::ClientSession() - token=%s", token);
	manager->add(this);
}


ADC::ClientSession::~ClientSession()
{
	while (extensions.size()) {
		char* feature = *extensions.begin();
		extensions.erase(extensions.begin());
		free(feature);
	}

	releaseTransfer();
	delete proxy;
	QDBG("~ADC::ClientSession()");
	
	delete socket; socket = 0;
	
	free(token);
	free(remote_sid);
	free(remote_cid);
}

void ADC::ClientSession::initialize()
{
	proxy             = 0;
	transfer          = 0;
	file              = 0;
	ownsFile          = false;
	chunk_offset      = 0;
	chunk_length      = 0;
	upload_approved   = false;
	download_approved = false;
	fetchingLeaves    = false;
	state             = StateNone;
}

const char* ADC::ClientSession::hubUrl() const
{
	if (!hubsession) return 0;
	QuickDC::Hub* hub = hubsession->getHub();
	return (hub) ? hub->getUrl() : 0;
}

void ADC::ClientSession::releaseTransfer()
{
	delete transfer; transfer = 0;

	/*
	 * An upload's file belongs to the share manager and must be left alone;
	 * a download's was made here, so it is ours to clean up.
	 */
	if (ownsFile) delete file;
	file = 0;
	ownsFile = false;

	chunk_offset = 0;
	chunk_length = 0;
}

void ADC::ClientSession::send(ADC::Command* cmd, bool more)
{
	proxy->send(cmd);
	if (!more)
		proxy->write();
}

void ADC::ClientSession::sendINF()
{
	ADC::Command* cmd = new ADC::Command(FOURCC('C','I','N','F'));
	cmd->addArgument("ID", ADC::CID::getInstance()->getCID());
	cmd->addArgument("VE", PRODUCT " " VERSION);
	if (token) cmd->addArgument("TO", token);
	send(cmd);
}

#define STATECHANGE(newstate, existingstate) \
	if (state == existingstate) { \
		state = newstate; \
	}

void ADC::ClientSession::onSUP()
{
	if (!isClient)
	{
		ADC::Command* cmd = new ADC::Command(FOURCC('C','S','U','P'));
		ADC::addSupportFlags(*cmd, true);
		send(cmd, true);
		sendINF();
		state = StateIdentify;
	}
}

void ADC::ClientSession::onINF(const char* cid, const char* remote_token)
{
	QDBG("ADC::ClientSession::onINF(): client=%d. got cid='%s', token='%s'", (int) isClient, cid, remote_token);

	/* Remember who this is; a FileSource is keyed on the CID, not the SID. */
	if (cid)
	{
		free(remote_cid);
		remote_cid = strdup(cid);
	}

	QuickDC::SecurityManager* security = QuickDC::Core::getInstance()->securityManager;

	/*
	 * Whichever side dialled, the token has to be one we issued and granted a
	 * clearance for. The peer quotes it back in their CINF when they dialled us.
	 */
	const char* job = (token) ? token : remote_token;

	if (job) {
		upload_approved = security->isAuthorized(QuickDC::Clearance::ADC, hubsession, socket, cid, job);
		QDBG("Authorized for upload: %s", upload_approved ? "yes" : "no");
	} else {
		QDBG("Neither side named a token; nothing to do on this connection");
	}

	/* The clearance covered setting this connection up, and it is now here. */
	if (cid && job)
		security->revoke(QuickDC::Clearance::ADC, hubsession, cid, job);

	/* The token is also what ties this connection to a queued download. */
	QuickDC::QueueManager* queue = QuickDC::Core::getInstance()->queue;

	if (queue && job && queue->lookupRequest(job))
	{
		if (!token) token = strdup(job);
		download_approved = true;
		QDBG("This connection is for a queued download (token=%s)", token);
	}

	if (isClient)
		sendINF();

	if (download_approved)
		requestNextChunk();
}

void ADC::ClientSession::requestNextChunk()
{
	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;

	if (!queue || !token)
	{
		QERR("requestNextChunk() with nothing to go on");
		return;
	}

	QuickDC::PendingRequest* request = queue->lookupRequest(token);
	if (!request)
	{
		QDBG("Nothing left in the queue for token '%s'; closing", token);
		socket->disconnect();
		return;
	}

	if (request->getKind() == QuickDC::PendingRequest::KindFilelist)
	{
		QuickDC::FilelistRequest* list = request->getFilelist();
		if (!list)
		{
			queue->onRequestFailed(token);
			socket->disconnect();
			return;
		}

		if (list->isActive())
		{
			/* We already asked; one list per connection is enough. */
			QDBG("The file list has already been requested; closing");
			socket->disconnect();
			return;
		}

		QDBG("Requesting the file list of '%s'", list->getClientID());
		list->setActive(true);

		/*
		 * Deliberately no ZL1: that is stream compression, and there is no
		 * decompressing counterpart to ZUpload on the receiving side yet. A
		 * bzip2 encoded list is a different matter - that is a file format,
		 * and QueueManager::onFilelistDownloaded() detects and unpacks it.
		 */
		ADC::Command* cmd = new ADC::Command(FOURCC('C','G','E','T'));
		cmd->addArgument("list");
		cmd->addArgument("/");
		cmd->addArgument("0");
		cmd->addArgument("-1");
		send(cmd);
		return;
	}

	QuickDC::QueueFile* qfile = request->getFile();
	if (!qfile || !qfile->getTTH())
	{
		QERR("A queued file without a TTH cannot be requested over ADC");
		queue->onRequestFailed(token);
		socket->disconnect();
		return;
	}

	/*
	 * Leaves first: they are what makes each chunk verifiable on its own, and
	 * they are only a few hundred bytes. Going without is not fatal, so a peer
	 * that refuses is not held against the download.
	 */
	if (!qfile->getLeaves() && !qfile->loadLeavesFromDisk())
	{
		QDBG("Asking for the leaf hashes of '%s'", qfile->getTTH());
		fetchingLeaves = true;

		ADC::Command* cmd = new ADC::Command(FOURCC('C','G','E','T'));
		cmd->addArgument("tthl");
		std::string name("TTH/");
		name += qfile->getTTH();
		cmd->addArgument(name.c_str());
		cmd->addArgument("0");
		cmd->addArgument("-1");
		send(cmd);
		return;
	}

	uint64_t offset = 0;
	uint64_t length = 0;
	if (!queue->reserveChunk(qfile, request->getSource(), offset, length))
	{
		QDBG("Nothing left to fetch of '%s' from this peer", qfile->getLocalFilename());
		queue->onRequestFinished(token);
		socket->disconnect();
		return;
	}

	request->setReservation(offset, length);
	chunk_offset = offset;
	chunk_length = (int64_t) length;

	char tthpath[64];
	char s_offset[32];
	char s_length[32];
	snprintf(tthpath, sizeof(tthpath), "TTH/%s", qfile->getTTH());
	snprintf(s_offset, sizeof(s_offset), "%llu", (unsigned long long) offset);
	snprintf(s_length, sizeof(s_length), "%llu", (unsigned long long) length);

	QDBG("Requesting %d bytes at offset %d of '%s'",
		(int) length, (int) offset, qfile->getLocalFilename());

	ADC::Command* cmd = new ADC::Command(FOURCC('C','G','E','T'));
	cmd->addArgument("file");
	cmd->addArgument(tthpath);
	cmd->addArgument(s_offset);
	cmd->addArgument(s_length);
	send(cmd);
}

void ADC::ClientSession::onGFI(const char*, uint32_t)
{
	if (upload_approved)
	{
		
	}
}

void ADC::ClientSession::onSTA(const char* code, const char* message)
{
	QDBG("The peer says '%s' (%s)", message ? message : "", code ? code : "");

	/* Severity 0 is informational and says nothing about our request. */
	if (!code || code[0] == '0') return;

	/*
	 * Anything else means we are not getting what we asked for. Hand the chunk
	 * back and blame the source, which also makes the scheduler wait before
	 * trying it again - it may simply be out of upload slots.
	 */
	if (download_approved && token)
	{
		QuickDC::QueueManager* queue = QuickDC::Core::getInstance()->queue;
		if (queue) queue->onRequestFailed(token);
	}

	socket->disconnect();
}

void ADC::ClientSession::onRES()
{

}


/**
 * Client session started, we speak first
 */
void ADC::ClientSession::onStartClient()
{
	QDBG("onStartClient()");
	ADC::Command* cmd = new ADC::Command(FOURCC('C','S','U','P'));
	ADC::addSupportFlags(*cmd, true);

	send(cmd);
	state = StateProtocol;
}

/**
 * Server session started, we wait for our turn
 */
void ADC::ClientSession::onStartServer()
{
	QDBG("onStartServer()");
}


void ADC::ClientSession::addSupport(const char* feature)
{
	QDBG("Adding support for '%s'", feature);
	extensions.insert(strdup(feature));
}

// FIXME: does this work?
bool ADC::ClientSession::supports(const char* feature)
{
	char* f = strdup(feature);
	bool found = extensions.count(f);
	free(f);
	return found;
}

// connection cancelled/closed
void ADC::ClientSession::onTransferStopped()
{
	QDBG("ADC::ClientSession::onTransferStopped()");

	if (!download_approved)
	{
		/* An upload finished; do nothing, just await a new request. */
		releaseTransfer();
		return;
	}

	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;

	/* Work out how it went before the transfer object goes away. */
	bool complete = (transfer && transfer->isFinished());
	bool wasFetchingLeaves = fetchingLeaves;
	fetchingLeaves = false;
	uint64_t offset = chunk_offset;
	uint64_t length = (chunk_length > 0) ? (uint64_t) chunk_length : 0;

	QuickDC::PendingRequest* request = (queue && token) ? queue->lookupRequest(token) : 0;

	/* A file list is not chunked, so it either arrived whole or not at all. */
	QuickDC::FilelistRequest* list = 0;
	if (request && request->getKind() == QuickDC::PendingRequest::KindFilelist)
		list = request->getFilelist();

	releaseTransfer();

	if (!queue || !token)
		return;

	if (list)
	{
		if (complete)
		{
			/* Shared with NMDC: unpack, read, and mine it for sources. */
			queue->onFilelistDownloaded(list, hubUrl(), remote_cid);
			queue->onRequestFinished(token);
			queue->removeFilelistRequest(list);
		}
		else
		{
			QDBG("The file list did not arrive in full");
			queue->onRequestFailed(token);
		}

		socket->disconnect();
		return;
	}

	if (!request)
		return;

	QuickDC::QueueFile* qfile = request->getFile();

	if (wasFetchingLeaves)
	{
		if (complete && qfile && !qfile->loadLeavesFromDisk())
			QDBG("The leaf hashes for '%s' were no use", qfile->getTTH());

		/* Either way, get on with the file itself. */
		requestNextChunk();
		return;
	}

	if (complete && length && !queue->verifyChunk(qfile, offset, length))
	{
		/*
		 * The bytes are on disk but do not match the tree, so this source gave
		 * us rubbish. Only this chunk is lost, which is the whole point of
		 * having the leaves.
		 */
		QERR("The chunk at %d of '%s' failed verification; discarding it",
			(int) offset, qfile ? qfile->getLocalFilename() : "");

		queue->releaseChunk(qfile, offset, length);
		request->clearReservation();
		queue->onRequestFailed(token);
		socket->disconnect();
		return;
	}

	if (complete && length)
	{
		queue->commitChunk(qfile, offset, length);
		request = queue->lookupRequest(token);
		if (request) request->clearReservation();

		/*
		 * commitChunk() may have completed the file, in which case the queue
		 * has already dropped the entry and this request with it.
		 */
		if (!queue->lookupRequest(token))
		{
			socket->disconnect();
			return;
		}

		/* Keep the connection and ask for the next piece. */
		requestNextChunk();
		return;
	}

	QDBG("The chunk at %d did not complete; handing it back", (int) offset);
	if (length) queue->releaseChunk(qfile, offset, length);
	request->clearReservation();
	queue->onRequestFailed(token);
	socket->disconnect();
}

#define FOURCC(a, b, c, d) ((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d))

Samurai::IO::File* ADC::ClientSession::openQueuedLeaves(const char* tth, uint64_t& length)
{
	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	return (queue) ? queue->openLeafFile(tth, length) : 0;
}

Samurai::IO::File* ADC::ClientSession::openQueuedFile(const char* tth, uint64_t offset, int64_t length, uint64_t& servable)
{
	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	return (queue) ? queue->openVerifiedRange(tth, offset, length, servable) : 0;
}

void ADC::ClientSession::onGET(const char* fn, uint32_t type, uint64_t offset, int64_t length, bool compress)
{
	QDBG("Requested file: '%s' (type=%d), offset=%d, length=%d", fn, (int) type, (int) offset, (int) length);
	if (!upload_approved)
	{
		QDBG("Client requested a file to be sent, however no upload is approved. Ignoring request...");
		return;
	}
	
	file = 0;
	uint64_t internal_offset = 0; // Our internal file position (might differ from what we send).
	uint64_t internal_length = 0; // Our internal length (might differ from the file's length, when only a part of it is being transferred).

	QuickDC::Share::Manager* sharemgr = QuickDC::Core::getInstance()->shares;
	QuickDC::Hash::Manager*   hashmgr = QuickDC::Core::getInstance()->hash;
	QuickDC::TransferManager*  xfrmgr = QuickDC::Core::getInstance()->transfers;

	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Network");
	int slots = config->getNumber("Slots", 3);
	bool slots_full = (slots > 0 && xfrmgr->countUploads() >= (size_t) slots);

	if (type == FOURCC('l','i','s','t'))
	{
		QDBG("looking up files.xml");
		file = sharemgr->getFile(supports(ADC_EXT_ZLIG) ? ADC_INDEX_FILE_BZ2 : ADC_INDEX_FILE);
		if (file) internal_length = file->size();
		slots_full = false;
	}
	else if (type == FOURCC('t','t','h','l'))
	{
		if (strlen(fn) == 43 && FOURCC(fn[0],fn[1],fn[2],fn[3]) == FOURCC('T','T','H','/'))
		{
			/* Leaves for a shared file live inside the hash cache, which the
			   hash manager owns; ours are a file of their own. */
			Samurai::IO::File* shared = sharemgr->getFileByTTH(&fn[4]);
			if (shared)
			{
				file = hashmgr->getLeafData(*shared, internal_offset, internal_length);
			}
			else
			{
				file = openQueuedLeaves(&fn[4], internal_length);
				if (file) ownsFile = true;
			}
		}

		slots_full = false;
	}
	else if (type == FOURCC('f','i','l','e'))
	{
		QDBG("looking up file: %s", fn);
		if (strlen(fn) == 43 && FOURCC(fn[0],fn[1],fn[2],fn[3]) == FOURCC('T','T','H','/'))
		{
			file = sharemgr->getFileByTTH(&fn[4]);

			if (!file)
			{
				file = openQueuedFile(&fn[4], offset, length, internal_length);
				if (file) ownsFile = true;
			}

			// Disable slots check if file is small enough
			if (slots_full && file && file->size() < 65536)
				slots_full = false;
		}
		else
		{
			// NOTE: I think this is wrong from the specification's point of view.
			//       These should really be fetched as a 'list' type (see above).
			if (!strcmp(fn, ADC_INDEX_FILE) || !strcmp(fn, ADC_INDEX_FILE_BZ2))
			{
				file = sharemgr->getFile(fn);
				slots_full = false;
			}
		}
	
		if (file && !internal_length) internal_length = file->size();

	} else {
		QDBG("Unexpected GET type: %x type, expected %x or %x.", type,  FOURCC('l','i','s','t'),  FOURCC('f','i','l','e'));
	}
	
	if (slots_full)
	{
		QDBG("Upload slot not available\n");
		ADC::Command* cmd = new ADC::Command(FOURCC('C','S','T','A'));
		cmd->addArgument(adc_status_code(ADC_STATUS_SEVERITY_ERROR, ADC_CODE_TRANSFER_SLOT_ERROR).c_str());
		cmd->addArgument(ADC_STR_TRANSFER_SLOT_ERROR);
		send(cmd);
		return;
	}
		
	if (file)
	{
		QDBG("Found file");
			
		chunk_offset = internal_offset + offset;
		chunk_length = length;
		if (chunk_length == -1)
			chunk_length = internal_length - offset;
		
		ADC::Command* cmd = new ADC::Command(FOURCC('C','S','N','D'));
		switch (type)
		{
			case FOURCC('l','i','s','t'):
				cmd->addArgument("list");
				break;
				
			case FOURCC('t','t','h','l'):
				cmd->addArgument("tthl");
				break;
				
			case FOURCC('f','i','l','e'):
				cmd->addArgument("file");
				break;
			
			default:
				return;
		}
		
		cmd->addArgument(fn);
		cmd->addArgument(quickdc_ulltoa(offset));
		cmd->addArgument(quickdc_ulltoa((uint64_t) chunk_length));
		if (compress)
		{
			cmd->addArgument("ZL", "1");
		}
		
		send(cmd);
		
		if (compress)
			transfer = new QuickDC::ZUpload(xfrmgr, this, proxy->socket, file, chunk_offset, chunk_length);
		else
			transfer = new QuickDC::Upload(xfrmgr, this, proxy->socket, file, chunk_offset, chunk_length);
	}
	else
	{
		QDBG("File not found");
		ADC::Command* cmd = new ADC::Command(FOURCC('C','S','T','A'));
		cmd->addArgument(adc_status_code(ADC_STATUS_SEVERITY_ERROR, ADC_CODE_TRANSFER_FILE_ERROR).c_str());
		cmd->addArgument(ADC_STR_TRANSFER_FILE_ERROR);
		send(cmd);
	}
}

void ADC::ClientSession::onSND(const char* filename, uint32_t type, uint64_t offset, int64_t length, bool compressed)
{
	QDBG("Receiving file (ADCSND): '%s', offset=%d, length=%d", filename, (int) offset, (int) length);

	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	QuickDC::TransferManager* xfrmgr = (core) ? core->transfers : 0;

	if (!download_approved || !queue || !xfrmgr || !token)
	{
		QERR("Unexpected ADCSND; no download is in progress on this connection");
		return;
	}

	if (compressed)
	{
		/*
		 * We never ask for ZL1, because there is no decompressing counterpart
		 * to ZUpload yet. A peer that sends it anyway is not something we can
		 * read, so do not pretend otherwise by writing the raw stream to disk.
		 */
		QERR("The peer sent a compressed stream, which is not supported");
		queue->onRequestFailed(token);
		socket->disconnect();
		return;
	}

	QuickDC::PendingRequest* request = queue->lookupRequest(token);
	if (!request)
	{
		QERR("ADCSND for a token we no longer know about ('%s')", token);
		socket->disconnect();
		return;
	}

	if (length <= 0)
	{
		QERR("The peer offered a nonsensical length (%d)", (int) length);
		queue->onRequestFailed(token);
		socket->disconnect();
		return;
	}

	/* There should be nothing running, but make sure before replacing it. */
	releaseTransfer();

	if (type == FOURCC('l','i','s','t'))
	{
		QuickDC::FilelistRequest* list = request->getFilelist();
		if (!list)
		{
			QERR("The peer sent a file list we did not ask for");
			queue->onRequestFailed(token);
			socket->disconnect();
			return;
		}

		file = new Samurai::IO::File(list->getTempFilename());
		ownsFile = true;

		/* Start from scratch; a list is not resumable. */
		Samurai::IO::File(list->getTempFilename()).remove();

		chunk_offset = 0;
		chunk_length = length;
		request->setReservation(0, (uint64_t) length);

		QuickDC::Download* download = new QuickDC::Download(xfrmgr, this, proxy->socket, file, 0, length);
		transfer = download;
		state = StateData;
		proxy->handOverTo(download);
		return;
	}

	if (type == FOURCC('t','t','h','l'))
	{
		QuickDC::QueueFile* qfile = request->getFile();
		if (!fetchingLeaves || !qfile)
		{
			QDBG("Ignoring an unsolicited tthl response");
			return;
		}

		char* name = qfile->makeLeafFilename();
		if (!name)
		{
			fetchingLeaves = false;
			requestNextChunk();
			return;
		}

		file = new Samurai::IO::File(name);
		ownsFile = true;
		free(name);

		Samurai::IO::File(file->getName()).remove();

		chunk_offset = 0;
		chunk_length = length;

		QuickDC::Download* download = new QuickDC::Download(xfrmgr, this, proxy->socket, file, 0, length);
		transfer = download;
		state = StateData;
		proxy->handOverTo(download);
		return;
	}

	/* type == file */
	QuickDC::QueueFile* qfile = request->getFile();
	if (!qfile)
	{
		QERR("The peer sent file data we did not ask for");
		queue->onRequestFailed(token);
		socket->disconnect();
		return;
	}

	/*
	 * A peer is allowed to answer with less than we asked for, but not with
	 * something outside the chunk we reserved - that would corrupt ranges we
	 * have already accounted for, or that someone else is fetching.
	 */
	if (!request->haveReservation()
		|| offset != request->getReservedOffset()
		|| (uint64_t) length > request->getReservedLength())
	{
		QERR("The peer answered with %d bytes at %d, which is not the chunk we reserved",
			(int) length, (int) offset);
		queue->onRequestFailed(token);
		socket->disconnect();
		return;
	}

	file = new Samurai::IO::File(qfile->getTempFilename());
	ownsFile = true;

	chunk_offset = offset;
	chunk_length = length;

	/*
	 * Hold the request to what the peer actually promised, which may be less
	 * than we reserved. The reservation in the chunk map still covers the
	 * larger range; committing the shorter one hands the remainder back.
	 */
	request->setReservation(offset, (uint64_t) length);

	QuickDC::Download* download = new QuickDC::Download(xfrmgr, this, proxy->socket, file, offset, length);
	transfer = download;
	state = StateData;
	proxy->handOverTo(download);
}

void ADC::ClientSession::EventHostLookup(const Samurai::IO::Net::Socket*) { }
void ADC::ClientSession::EventHostFound(const Samurai::IO::Net::Socket*) { }

void ADC::ClientSession::EventConnecting(const Samurai::IO::Net::Socket* sock)
{
	QDBG("Connecting to %s", sock->getAddress()->toString());
}

void ADC::ClientSession::EventConnected(const Samurai::IO::Net::Socket*)
{
	isClient = true;
	manager->add(this);

	if (!secure)
	{
		onStartClient();
		return;
	}

	/*
	 * The peer asked for ADCS, so nothing may be sent in the clear: TLS comes
	 * up first and the protocol starts from EventTLSConnected(). Same shape as
	 * ADC::HubSession::EventConnected().
	 */
#ifdef SSL_SUPPORT
	QDBG("Peer wants ADCS; starting the TLS handshake");
	if (!socket->TLSInitialize(false))
	{
		QERR("Unable to initialize TLS for the peer connection");
		abandonDownload();
		manager->remove(this);
		return;
	}
	socket->TLSsendHandshake();
#else
	QERR("The peer asked for ADCS, but TLS support is not built in");
	abandonDownload();
	manager->remove(this);
#endif
}

void ADC::ClientSession::EventTLSConnected(const Samurai::IO::Net::Socket*)
{
	QDBG("TLS established with the peer");
	if (secure) onStartClient();
}

void ADC::ClientSession::EventTLSDisconnected(const Samurai::IO::Net::Socket*)
{
	QDBG("TLS shut down on the peer connection");
}

/**
 * The connection is going away. Anything we reserved but did not receive has
 * to go back to the queue, or that range of the file would be blocked for the
 * rest of the run.
 */
void ADC::ClientSession::abandonDownload()
{
	if (!download_approved || !token) return;

	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	if (!queue) return;

	QuickDC::PendingRequest* request = queue->lookupRequest(token);
	if (!request) return;

	/*
	 * A transfer that ran to completion has already been dealt with in
	 * onTransferStopped(), which drops the request; getting here with one
	 * still registered means it did not finish.
	 */
	QDBG("Abandoning the download for token '%s'", token);
	queue->onRequestFailed(token);
}

void ADC::ClientSession::EventTimeout(const Samurai::IO::Net::Socket*)
{
	QDBG("Peer timeout");
	abandonDownload();
	manager->remove(this);
}

void ADC::ClientSession::EventDisconnected(const Samurai::IO::Net::Socket*)
{
	QDBG("Disconnected from peer");
	abandonDownload();
	manager->remove(this);
}

void ADC::ClientSession::EventDataAvailable(const Samurai::IO::Net::Socket*)
{
	if (transfer && transfer->isDownload())
	{
		transfer->exec();
	}
	else
	{
		proxy->read();
	}
}

void ADC::ClientSession::EventCanWrite(const Samurai::IO::Net::Socket*)
{
#ifdef DATADUMP
	QDBG("DC::Connection is ready to write");
#endif
	if (transfer && transfer->isUpload())
	{
		transfer->exec();
	}
	else
	{
		proxy->write();
	}
}

void ADC::ClientSession::EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char* msg)
{
	if (hubsession)
		hubsession->sendPeerError(remote_sid, "242", msg, token, ADC_COMPLIANCE);
	QERR("Error: %s", msg);
	abandonDownload();
	manager->remove(this);
}


