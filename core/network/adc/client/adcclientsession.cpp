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
#include "network/keyprint.h"
#include "network/leafhashes.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/adccommands.h"
#include "network/adc/parser.h"
#include "network/adc/cid.h"
#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <string>
#include <string_view>

ADC::ClientSession::ClientSession(Samurai::IO::Net::URL* url, ADC::HubSession* hubsession_) :
	QuickDC::Connection(url, "ADC client"),
	hubsession(hubsession_),
	isClient(true),
	secure(false)
{
	initialize();
	proxy = new ADC::CommandProxy(socket, this);
	QDBG("ADC::ClientSession()");
}

ADC::ClientSession::ClientSession(std::shared_ptr<Samurai::IO::Net::Socket> sock, ADC::HubSession* hubsession_, const char* token_, const char* sid_, bool secure_, const char* keyprint_) :
	QuickDC::Connection(std::move(sock), secure_ ? "ADCS client" : "ADC client"),
	hubsession(hubsession_),
	isClient(false),
	secure(secure_)
{
	if (token_) token = token_;
	if (sid_) remote_sid = sid_;
	if (keyprint_) expected_keyprint = keyprint_;

	initialize();
	proxy = new ADC::CommandProxy(socket, this);
	QDBG("ADC::ClientSession() - token=%s", token.c_str());
	manager->add(this);
}


ADC::ClientSession::~ClientSession()
{
	releaseTransfer();
	delete proxy;
	QDBG("~ADC::ClientSession()");
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
	inf_sent          = false;
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
	if (inf_sent) return;
	inf_sent = true;

	ADC::Command* cmd = new ADC::Command(FOURCC('C','I','N','F'));
	cmd->addArgument("ID", ADC::CID::getInstance()->getCID());
	cmd->addArgument("VE", PRODUCT " " VERSION);
	if (!token.empty()) cmd->addArgument("TO", token);
	send(cmd);
}

#define STATECHANGE(newstate, existingstate) \
	if (state == existingstate) { \
		state = newstate; \
	}

/*
 * The support exchange is over, so both sides say who they are: the side that was
 * dialled answers the CSUP first, and each then sends its CINF without waiting for
 * the other's - neither can, since both are waiting for the same thing.
 */
void ADC::ClientSession::onSUP()
{
	if (!isClient)
	{
		ADC::Command* cmd = new ADC::Command(FOURCC('C','S','U','P'));
		ADC::addSupportFlags(*cmd, true);
		send(cmd, true);
	}

	sendINF();
	state = StateIdentify;
}

void ADC::ClientSession::onINF(const char* cid, const char* remote_token)
{
	QDBG("ADC::ClientSession::onINF(): client=%d. got cid='%s', token='%s'", static_cast<int>(isClient), cid, remote_token);

	/* Remember who this is; a FileSource is keyed on the CID, not the SID. */
	if (cid) remote_cid = cid;

	QuickDC::SecurityManager* security = QuickDC::Core::getInstance()->securityManager;

	/*
	 * Whichever side dialled, the token has to be one we issued and granted a
	 * clearance for. The peer quotes it back in their CINF when they dialled us.
	 */
	const char* job = (!token.empty()) ? token.c_str() : remote_token;

	if (job) {
		upload_approved = security->isAuthorized(QuickDC::Clearance::ADC, hubsession, socket.get(), cid, job);
		QDBG("Authorized for upload: %s", upload_approved ? "yes" : "no");
	} else {
		QDBG("Neither side named a token; nothing to do on this connection");
	}

	/*
	 * The clearance covered setting this connection up, and it is now here - so
	 * it is spent, and one connection is all it authorises.
	 *
	 * Spent only when it authorised this connection. revoke() matches the token
	 * as well as the CID, so a peer quoting the wrong one already matched no
	 * clearance and took nothing with it; this says so at the call rather than
	 * leaving it resting on how revoke() happens to match.
	 */
	if (upload_approved && cid && job)
		security->revoke(QuickDC::Clearance::ADC, hubsession, cid, job);

	/* The token is also what ties this connection to a queued download. */
	QuickDC::QueueManager* queue = QuickDC::Core::getInstance()->queue;

	if (queue && job && queue->lookupRequest(job))
	{
		if (token.empty()) token = job;
		download_approved = true;
		QDBG("This connection is for a queued download (token=%s)", token.c_str());
	}

	/* A peer that identified without answering our CSUP still gets an answer. */
	sendINF();

	if (download_approved)
		requestNextChunk();
}

void ADC::ClientSession::requestNextChunk()
{
	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;

	if (!queue || token.empty())
	{
		QERR("requestNextChunk() with nothing to go on");
		return;
	}

	QuickDC::PendingRequest* request = queue->lookupRequest(token.c_str());
	if (!request)
	{
		QDBG("Nothing left in the queue for token '%s'; closing", token.c_str());
		socket->disconnect();
		return;
	}

	if (request->getKind() == QuickDC::PendingRequest::KindFilelist)
	{
		QuickDC::FilelistRequest* list = request->getFilelist();
		if (!list)
		{
			queue->onRequestFailed(token.c_str());
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
		 * The whole share, named as the rootless "files.xml.bz2" in the file
		 * namespace, rather than asked for as a list.
		 *
		 * A list request names one directory and gets one directory: a peer
		 * that answers it to the letter sends that directory's immediate
		 * subdirectories, each marked Incomplete="1" and holding nothing, and
		 * browsing further means asking again for every folder opened. Our own
		 * upload side answers a list with the entire file list whatever was
		 * asked for, which is not something to expect of anyone else.
		 *
		 * Deliberately no ZL1: that is stream compression, and there is no
		 * decompressing counterpart to ZUpload on the receiving side yet. A
		 * bzip2 encoded list is a different matter - that is a file format,
		 * and QueueManager::onFilelistDownloaded() detects and unpacks it.
		 */
		ADC::Command* cmd = new ADC::Command(FOURCC('C','G','E','T'));
		cmd->addArgument("file");
		cmd->addArgument(ADC_INDEX_FILE_BZ2);
		cmd->addArgument("0");
		cmd->addArgument("-1");
		send(cmd);
		return;
	}

	QuickDC::QueueFile* qfile = request->getFile();
	if (!qfile || !qfile->getTTH())
	{
		QERR("A queued file without a TTH cannot be requested over ADC");
		queue->onRequestFailed(token.c_str());
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
		queue->onRequestFinished(token.c_str());
		socket->disconnect();
		return;
	}

	request->setReservation(offset, length);
	chunk_offset = offset;
	chunk_length = static_cast<int64_t>(length);

	const std::string tthpath = "TTH/" + std::string(qfile->getTTH());

	QDBG("Requesting %d bytes at offset %d of '%s'",
		static_cast<int>(length), static_cast<int>(offset), qfile->getLocalFilename());

	ADC::Command* cmd = new ADC::Command(FOURCC('C','G','E','T'));
	cmd->addArgument("file");
	cmd->addArgument(tthpath.c_str());
	cmd->addArgument(std::to_string(offset).c_str());
	cmd->addArgument(std::to_string(length).c_str());
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
	(void) message;

	/* Severity 0 is informational and says nothing about our request. */
	if (!code || code[0] == '0') return;

	/*
	 * Anything else means we are not getting what we asked for. Hand the chunk
	 * back and blame the source, which also makes the scheduler wait before
	 * trying it again - it may simply be out of upload slots.
	 */
	if (download_approved && !token.empty())
	{
		QuickDC::QueueManager* queue = QuickDC::Core::getInstance()->queue;
		if (queue) queue->onRequestFailed(token.c_str());
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
	if (feature) extensions.insert(feature);
}

bool ADC::ClientSession::supports(const char* feature)
{
	return feature && extensions.count(feature) != 0;
}

// connection cancelled/closed
void ADC::ClientSession::onTransferStopped(QuickDC::Transfer* which)
{
	QDBG("ADC::ClientSession::onTransferStopped()");

	/*
	 * Only ever the transfer that stopped. A connection can hold more than one over
	 * its life - a peer asking us for a file while we are downloading from it is
	 * enough - and releasing whatever we happen to be holding would delete an object
	 * the socket is still feeding. One that is not ours to hold any more is still
	 * ours to free.
	 */
	if (which && which != transfer)
	{
		QDBG("A transfer stopped that this connection has already moved on from");
		delete which;
		return;
	}

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
	uint64_t length = (chunk_length > 0) ? static_cast<uint64_t>(chunk_length) : 0;

	QuickDC::PendingRequest* request = (queue && !token.empty()) ? queue->lookupRequest(token.c_str()) : 0;

	/* A file list is not chunked, so it either arrived whole or not at all. */
	QuickDC::FilelistRequest* list = 0;
	if (request && request->getKind() == QuickDC::PendingRequest::KindFilelist)
		list = request->getFilelist();

	releaseTransfer();

	if (!queue || token.empty())
		return;

	if (list)
	{
		if (complete)
		{
			/* Shared with NMDC: unpack, read, and mine it for sources. */
			queue->onFilelistDownloaded(list, hubUrl(), remote_cid.empty() ? 0 : remote_cid.c_str());
			queue->onRequestFinished(token.c_str());
			queue->removeFilelistRequest(list);
		}
		else
		{
			QDBG("The file list did not arrive in full");
			queue->onRequestFailed(token.c_str());
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
			static_cast<int>(offset), qfile ? qfile->getLocalFilename() : "");

		queue->releaseChunk(qfile, offset, length);
		request->clearReservation();
		queue->onRequestFailed(token.c_str());
		socket->disconnect();
		return;
	}

	if (complete && length)
	{
		queue->commitChunk(qfile, offset, length);
		request = queue->lookupRequest(token.c_str());
		if (request) request->clearReservation();

		/*
		 * commitChunk() may have completed the file, in which case the queue
		 * has already dropped the entry and this request with it.
		 */
		if (!queue->lookupRequest(token.c_str()))
		{
			socket->disconnect();
			return;
		}

		/* Keep the connection and ask for the next piece. */
		requestNextChunk();
		return;
	}

	QDBG("The chunk at %d did not complete; handing it back", static_cast<int>(offset));
	if (length) queue->releaseChunk(qfile, offset, length);
	request->clearReservation();
	queue->onRequestFailed(token.c_str());
	socket->disconnect();
}

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
	QDBG("Requested file: '%s' (type=%d), offset=%d, length=%d", fn, static_cast<int>(type), static_cast<int>(offset), static_cast<int>(length));
	if (!upload_approved)
	{
		QDBG("Client requested a file to be sent, however no upload is approved. Ignoring request...");
		return;
	}
	
	/*
	 * One transfer at a time on a connection. A peer that asks for a file while
	 * something is already moving - which it may, since a connection can be
	 * authorised both ways at once - had the running transfer dropped where it
	 * stood, taking the download it belonged to with it. Refused as busy instead,
	 * which is what being busy is.
	 */
	if (transfer)
	{
		QDBG("A transfer is already running on this connection; refusing");
		ADC::Command* cmd = new ADC::Command(FOURCC('C','S','T','A'));
		cmd->addArgument(adc_status_code(ADC_STATUS_SEVERITY_ERROR, ADC_CODE_TRANSFER_SLOT_ERROR).c_str());
		cmd->addArgument(ADC_STR_TRANSFER_SLOT_ERROR);
		send(cmd);
		return;
	}

	/*
	 * And whatever the last request left behind, before this one takes the members
	 * over. Clearing 'file' on its own leaked it when it was ours.
	 */
	releaseTransfer();

	uint64_t internal_offset = 0; // Our internal file position (might differ from what we send).
	uint64_t internal_length = 0; // Our internal length (might differ from the file's length, when only a part of it is being transferred).

	QuickDC::Share::Manager* sharemgr = QuickDC::Core::getInstance()->shares;
	QuickDC::Hash::Manager*   hashmgr = QuickDC::Core::getInstance()->hash;
	QuickDC::TransferManager*  xfrmgr = QuickDC::Core::getInstance()->transfers;

	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Network");
	int slots = config->getNumber("Slots", 3);
	bool slots_full = (slots > 0 && xfrmgr->countUploads() >= static_cast<size_t>(slots));

	if (type == FOURCC('l','i','s','t'))
	{
		QDBG("looking up files.xml");

		/*
		 * Plain, whatever the peer says it can read: a list response carries the
		 * XML itself, and the bzip2 form is a file that a peer fetches by name
		 * instead. ZLIG is stream compression, which is a different thing again
		 * and is asked for per transfer.
		 */
		file = sharemgr->getFile(ADC_INDEX_FILE);
		if (file) internal_length = file->size();
		slots_full = false;
	}
	else if (type == FOURCC('t','t','h','l'))
	{
		if (strlen(fn) == 43 && FOURCC(static_cast<uint32_t>(fn[0]), static_cast<uint32_t>(fn[1]),
			static_cast<uint32_t>(fn[2]), static_cast<uint32_t>(fn[3])) == FOURCC('T','T','H','/'))
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
		if (strlen(fn) == 43 && FOURCC(static_cast<uint32_t>(fn[0]), static_cast<uint32_t>(fn[1]),
			static_cast<uint32_t>(fn[2]), static_cast<uint32_t>(fn[3])) == FOURCC('T','T','H','/'))
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
			const std::string_view name(fn ? fn : "");
			if (name == ADC_INDEX_FILE || name == ADC_INDEX_FILE_BZ2)
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
		cmd->addArgument(std::to_string(offset).c_str());
		cmd->addArgument(std::to_string(static_cast<uint64_t>(chunk_length)).c_str());
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
	QDBG("Receiving file (ADCSND): '%s', offset=%d, length=%d", filename, static_cast<int>(offset), static_cast<int>(length));
	(void) filename;

	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	QuickDC::TransferManager* xfrmgr = (core) ? core->transfers : 0;

	if (!download_approved || !queue || !xfrmgr || token.empty())
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
		queue->onRequestFailed(token.c_str());
		socket->disconnect();
		return;
	}

	QuickDC::PendingRequest* request = queue->lookupRequest(token.c_str());
	if (!request)
	{
		QERR("ADCSND for a token we no longer know about ('%s')", token.c_str());
		socket->disconnect();
		return;
	}

	if (length <= 0)
	{
		QERR("The peer offered a nonsensical length (%d)", static_cast<int>(length));
		queue->onRequestFailed(token.c_str());
		socket->disconnect();
		return;
	}

	/* There should be nothing running, but make sure before replacing it. */
	releaseTransfer();

	/*
	 * What was asked for decides this, not what the answer calls itself: the
	 * whole share is fetched as a file, and a peer answering an older style
	 * list request is sending the same thing under another name.
	 */
	if (request->getKind() == QuickDC::PendingRequest::KindFilelist)
	{
		QuickDC::FilelistRequest* list = request->getFilelist();
		if (!list)
		{
			QERR("The peer sent a file list we did not ask for");
			queue->onRequestFailed(token.c_str());
			socket->disconnect();
			return;
		}

		file = new Samurai::IO::File(list->getTempFilename());
		ownsFile = true;

		/* Start from scratch; a list is not resumable. */
		Samurai::IO::File(list->getTempFilename()).remove();

		chunk_offset = 0;
		chunk_length = length;
		request->setReservation(0, static_cast<uint64_t>(length));

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
		queue->onRequestFailed(token.c_str());
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
		|| static_cast<uint64_t>(length) > request->getReservedLength())
	{
		QERR("The peer answered with %d bytes at %d, which is not the chunk we reserved",
			static_cast<int>(length), static_cast<int>(offset));
		queue->onRequestFailed(token.c_str());
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
	request->setReservation(offset, static_cast<uint64_t>(length));

	QuickDC::Download* download = new QuickDC::Download(xfrmgr, this, proxy->socket, file, offset, length);
	transfer = download;
	state = StateData;
	proxy->handOverTo(download);
}

void ADC::ClientSession::EventHostLookup(const Samurai::IO::Net::Socket*) { }
void ADC::ClientSession::EventHostFound(const Samurai::IO::Net::Socket*) { }

void ADC::ClientSession::EventConnecting(const Samurai::IO::Net::Socket* sock)
{
	/* Asked inside the log call, because getAddress() answers null until there is
	   a peer - and a socket that is still connecting may never get one. */
	QDBG("Connecting to %s", sock && sock->getAddress()
		? sock->getAddress()->toString().c_str() : "(unknown)");
	(void) sock;
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
	QDBG("Peer wants ADCS; starting the TLS handshake");

	/* A peer's certificate is vouched for by the keyprint the hub published for
	   them, checked in EventTLSConnected(), and by nothing a CA ever signed. */
	socket->TLSsetAllowUntrusted(true);
	if (!socket->TLSInitialize(false))
	{
		QERR("Unable to initialize TLS for the peer connection");
		abandonDownload();
		manager->remove(this);
		return;
	}
	socket->TLSsendHandshake();
}

void ADC::ClientSession::EventTLSConnected(const Samurai::IO::Net::Socket*)
{
	QDBG("TLS established with the peer");

	/*
	 * KEYP: the hub told us which certificate this peer holds, and it did so
	 * over a connection we had already verified. Anything else answering here
	 * is not the peer we asked for, so nothing may be said to it.
	 */
	if (expected_keyprint)
	{
		std::string actual;

		if (!QuickDC::Keyprint::ofPeer(socket.get(), actual)
			|| !QuickDC::Keyprint::equals(expected_keyprint->c_str(), actual.c_str()))
		{
			QERR("Crypto error: the peer's certificate does not match the keyprint "
			     "the hub published for them; aborting");
			socket->disconnect();
			return;
		}

		QDBG("The peer's certificate matches the keyprint the hub published");
	}

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
	if (!download_approved || token.empty()) return;

	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	if (!queue) return;

	QuickDC::PendingRequest* request = queue->lookupRequest(token.c_str());
	if (!request) return;

	/*
	 * A transfer that ran to completion has already been dealt with in
	 * onTransferStopped(), which drops the request; getting here with one
	 * still registered means it did not finish.
	 */
	QDBG("Abandoning the download for token '%s'", token.c_str());
	queue->onRequestFailed(token.c_str());
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

void ADC::ClientSession::EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError error, const char* msg)
{
	/*
	 * What the peer is told is a status code and a phrase of ours. 'msg' is the C
	 * library's text for our own errno, and it went out in a DSTA relayed through
	 * the hub - telling whoever asked for this connection what this host's
	 * operating system is and what state it is in. The code is what the peer needs
	 * to know the connection it asked for is not happening; the errno is not.
	 */
	QERR("Peer connection failed with socket error %d: %s",
		static_cast<int>(error), msg ? msg : "no detail given");

	if (hubsession)
		hubsession->sendPeerError(remote_sid.empty() ? 0 : remote_sid.c_str(),
			adc_status_code(ADC_STATUS_SEVERITY_FATAL, ADC_CODE_PROTOCOL_CONNECT_ERROR).c_str(),
			ADC_STR_CONNECTION_FAILED, token.empty() ? 0 : token.c_str(), ADC_COMPLIANCE);

	abandonDownload();
	manager->remove(this);
}


