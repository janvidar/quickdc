/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <algorithm>
#include <stdio.h>
#include <string>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "network/dc/clientsession.h"
#include "network/dc/clientcommands.h"
#include "network/usermanager.h"
#include "config/preferences.h"
#include "api/hub.h"
#include "api/core.h"
#include "share/sharemanager.h"
#include "hash/hashmanager.h"
#include "network/connectionmanager.h"
#include "network/hubmanager.h"
#include "network/securitymanager.h"
#include "network/transfer.h"
#include "network/transfermanager.h"
#include "network/downloadqueue.h"
#include "network/filesource.h"
#include "network/dc/commands.h"
#include "network/dc/hubsession.h"
#include "network/dc/dccommandproxy.h"
#include "network/dc/lock.h"
#include <samurai/io/file.h>
#include <samurai/util/random.h>

#define STATECHANGE(newstate, existingstate) \
	if (state == existingstate) \
	{ \
		state = newstate; \
	}

DC::ClientSession::ClientSession(QuickDC::Core& core_, Samurai::IO::Net::URL* url, DC::HubSession* hubsession_) :
	QuickDC::Connection(core_, url, "NMDC client"),
	hubsession(hubsession_),
	isClient(true)
{
	initialize();
	proxy = new DC::CommandProxy(socket, this);
	QDBG("DC::ClientSession()");
}


DC::ClientSession::ClientSession(QuickDC::Core& core_, std::shared_ptr<Samurai::IO::Net::Socket> sock, DC::HubSession* hubsession_) :
	QuickDC::Connection(core_, std::move(sock), "NMDC client"),
	hubsession(hubsession_),
	isClient(false)
{
	initialize();
	proxy = new DC::CommandProxy(socket, this);
	QDBG("DC::ClientSession()");
	manager->add(this);
}


DC::ClientSession::~ClientSession()
{
	releaseTransfer();

	delete proxy;

	QDBG("~DC::ClientSession()");
}


void DC::ClientSession::initialize()
{
	my_prio          = 0;
	remote_prio      = 0;
	proxy            = 0;
	transfer         = 0;
	file             = 0;
	ownsFile         = false;
	chunk_offset     = 0;
	chunk_length     = 0;
	challenged       = false;
	upload_approved  = false;
	download_approved = false;
	extendedProtocol = false;
	state            = StateNone;
	remote_direction = DirNone;
	my_direction     = DirNone;
}

void DC::ClientSession::releaseTransfer()
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

const char* DC::ClientSession::hubUrl() const
{
	if (!hubsession) return 0;
	QuickDC::Hub* hub = hubsession->getHub();
	return (hub) ? hub->getUrl() : 0;
}

bool DC::ClientSession::wantToDownload()
{
	QuickDC::Core* core = &getCore();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	if (!queue) return false;

	if (remote_nickname.empty()) return false;

	const char* hub = hubUrl();
	if (!hub) return false;

	/*
	 * On NMDC the user is the only handle we have: $Direction has no room for
	 * a token, so which nick we are talking to is what identifies the job.
	 */
	return (queue->lookupRequestBySource(hub, remote_nickname.c_str()) != 0);
}

void DC::ClientSession::abandonDownload()
{
	if (!download_approved || token.empty()) return;

	QuickDC::Core* core = &getCore();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	if (!queue) return;

	/*
	 * A transfer that ran to completion has already been dealt with in
	 * onTransferStopped(), which drops the request; finding one still
	 * registered means it did not finish.
	 */
	if (!queue->lookupRequest(token.c_str())) return;

	QDBG("Abandoning the download for token '%s'", token.c_str());
	queue->onRequestFailed(token.c_str());
}

void DC::ClientSession::send(DC::Command* cmd, bool more)
{
	proxy->send(cmd);
	
	if (!more)
		proxy->write();

	// resetIdleTimer();
}



/**
 * Client session started, we speak first
 */
void DC::ClientSession::onStartClient()
{
	QDBG("onStartClient()");
	isClient = true;
	sendChallenge();
	// STATECHANGE(StateConnected, StateNone);
}


/**
 * Server session started, we wait for our turn
 */
void DC::ClientSession::onStartServer()
{
	QDBG("onStartServer()");
	isClient = false;
	STATECHANGE(StateConnected, StateNone);
}


void DC::ClientSession::sendChallenge()
{
	if (challenged) return;

	/*
	 * An inbound peer connection is constructed without a hub session - the
	 * protocol probe has only seen "$MyNick " at that point and has no hub to
	 * attribute it to - so there is no local user to name here. The nick is the
	 * whole point of the challenge, so there is nothing to send without one.
	 */
	const QuickDC::User* me = hubsession ? hubsession->getLocalUser() : 0;
	if (!me || !me->getNick())
	{
		QERR("No local user to challenge with; dropping the connection");
		proxy->close();
		return;
	}

	const std::string my_lock = Lock::createLock();
	if (my_lock.empty())
	{
		QERR("No lock to challenge with; dropping the connection");
		proxy->close();
		return;
	}

	const std::string my_pk = Lock::createPk();
	my_key = DC::Lock::calculateKey(my_lock);

	send(new DC::SendMyNick(me->getNick()), true);
	send(new DC::SendLock(my_lock.c_str(), my_pk.c_str()));

	challenged = true;
}


void DC::ClientSession::sendDirection()
{
	if (isClient && !my_direction)
	{
		STATECHANGE(StateLock, StateNick);
	}
	
	if (state == StateLock && !my_direction)
	{
		/*
		 * Ask to download only when the queue actually wants something from
		 * this user; otherwise offer to serve, which is what a peer that
		 * dialled us is usually after.
		 */
		my_direction = wantToDownload() ? DirDownload : DirUpload;
		my_prio = Samurai::Util::pseudoRandom(1000, 32767);

		QDBG("Asking for a %s (priority %d)",
			my_direction == DirDownload ? "download" : "upload", static_cast<int>(my_prio));

		if (extendedProtocol)
		{
			send(new DC::SendClientSupports(), true);

		}
		
		send(new DC::SendDirection( my_direction == DirDownload, my_prio ));
		
		STATECHANGE(StateDirection, StateLock);
	}
}


void DC::ClientSession::sendKey()
{
	if (state == StateDirection)
	{
		send(new DC::SendKey(remote_key.c_str(), remote_key.size()));
		STATECHANGE(StateIdle, StateDirection);
	}
}


void DC::ClientSession::sendErrorFileUnavailable()
{
	if (upload_approved) {
		QDBG("File not found");
		send(new DC::SendErrorFileNotAvailable());
	}
}


void DC::ClientSession::sendFileSize()
{
	if (upload_approved && file)
	{
		send(new DC::SendFileLength(file->size()));
	}
}


void DC::ClientSession::onLock(const char* lock_, const char* pk_)
{
	if (!remote_lock.empty() || !remote_pk.empty()) return;

	/*
	 * calculateKey() has nothing to derive a key from below three characters, and
	 * a peer we cannot answer is one there is no handshake with.
	 */
	if (!lock_ || strlen(lock_) < 3)
	{
		QERR("Peer sent a lock too short to answer; dropping the connection");
		proxy->close();
		return;
	}

	remote_lock = lock_;
	remote_pk = pk_ ? pk_ : "";

	extendedProtocol = remote_lock.starts_with("EXTENDEDPROTOCOL");
	remote_key = Lock::calculateKey(remote_lock);
	if (remote_key.empty())
	{
		QERR("Unable to derive a key from the peer's lock; dropping the connection");
		proxy->close();
		return;
	}

	if (!isClient)
	{
		sendChallenge();
		STATECHANGE(StateLock, StateNick);
		sendDirection();
		sendKey();
	}
}


/*
 * $Key: the peer's answer to the lock we challenged them with.
 *
 * It authenticates nobody, and cannot: the derivation is unkeyed and fully
 * invertible, so anyone who saw the lock can compute the answer, and the lock
 * went out in the clear. What checking it does establish is that the peer read
 * the challenge we sent and speaks the protocol - so a wrong answer is a peer
 * replaying someone else's handshake or not following it at all, and neither is
 * something to serve a file to. Not compared in constant time, deliberately:
 * there is no secret here to leak the shape of.
 */
void DC::ClientSession::onKey(const char* key_)
{
	if (my_key.empty())
	{
		QERR("A key arrived before we issued a lock; dropping the connection");
		proxy->close();
		return;
	}

	if (!key_ || my_key != key_)
	{
		QERR("The peer answered our lock with the wrong key; dropping the connection");
		proxy->close();
		return;
	}

	if (isClient)
	{
		sendDirection();
		sendKey();
	}

	/*
	 * NMDC direction negotiation: whoever asked to download gets to. If both
	 * asked, the higher priority wins and the loser serves instead. If we
	 * offered to upload we serve either way.
	 */
	bool weDownload = false;
	if (my_direction == DirDownload)
	{
		if (remote_direction == DirDownload)
			weDownload = (my_prio > remote_prio);
		else
			weDownload = true;
	}

	download_approved = weDownload;

	/*
	 * Winning the direction negotiation is not authorisation to be served. The
	 * clearance says this nick is one we asked to connect, which is the only
	 * thing NMDC gives us to check - see SecurityManager::isAuthorized(). Being
	 * asked to upload by a peer we never invited is refused instead.
	 */
	upload_approved = false;
	if (!weDownload)
	{
		QuickDC::SecurityManager* security = getCore().securityManager;
		upload_approved = security && security->isAuthorized(QuickDC::Clearance::NMDC,
			hubsession, proxy->socket.get(), getNick(), 0);

		if (!upload_approved)
		{
			QERR("Refusing to serve '%s': no connection was requested from them",
				remote_nickname.empty() ? "(unidentified)" : remote_nickname.c_str());
			proxy->close();
			return;
		}

		/*
		 * Consumed, as on the ADC side. It matters more here: a clearance is
		 * matched on the nickname alone, which every client on the hub was told,
		 * so what bounds the damage is that it authorises one connection rather
		 * than every connection until the process ends.
		 */
		security->revoke(QuickDC::Clearance::NMDC, hubsession, getNick(), 0);
	}

	if (weDownload)
	{
		QDBG("We won the direction negotiation; requesting data.");
		requestNextChunk();
		return;
	}

	QDBG("Upload approved.");

	if (my_direction == DirDownload)
	{
		/*
		 * We wanted to download and lost the coin toss. Release the job now
		 * rather than letting it time out, so the scheduler can try again
		 * somewhere else instead of treating the source as busy for minutes.
		 * The peer did answer us, so this is not counted as a failure.
		 */
		QuickDC::Core* core = &getCore();
		QuickDC::QueueManager* queue = (core) ? core->queue : 0;
		const char* hub = hubUrl();

		if (queue && hub && !remote_nickname.empty())
		{
			QuickDC::PendingRequest* request = queue->lookupRequestBySource(hub, remote_nickname.c_str());
			if (request)
			{
				QDBG("Lost the direction negotiation; handing the job back");
				queue->onRequestFinished(request->getToken());
			}
		}
	}
}

void DC::ClientSession::requestNextChunk()
{
	QuickDC::Core* core = &getCore();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;

	if (!queue)
	{
		proxy->close();
		return;
	}

	/* NMDC carries no token, so find the job by who we are talking to once. */
	if (token.empty())
	{
		const char* hub = hubUrl();
		QuickDC::PendingRequest* found = (hub && !remote_nickname.empty())
			? queue->lookupRequestBySource(hub, remote_nickname.c_str()) : 0;

		if (!found)
		{
			QDBG("Nothing queued for '%s'; closing", remote_nickname.c_str());
			proxy->close();
			return;
		}

		token = found->getToken();
	}

	QuickDC::PendingRequest* request = queue->lookupRequest(token.c_str());
	if (!request)
	{
		QDBG("Nothing left in the queue for token '%s'; closing", token.c_str());
		proxy->close();
		return;
	}

	/*
	 * The legacy "$Get {file}${offset}" has no way to name a file by hash and
	 * cannot resume a segment cleanly, so only the ADCGet extension is used.
	 */
	if (!extendedProtocol || !supports("ADCGet"))
	{
		QERR("'%s' does not support $ADCGET; the legacy $Get is not implemented",
			remote_nickname.c_str());
		queue->onRequestFailed(token.c_str());
		proxy->close();
		return;
	}

	if (request->getKind() == QuickDC::PendingRequest::KindFilelist)
	{
		QuickDC::FilelistRequest* list = request->getFilelist();
		if (!list)
		{
			queue->onRequestFailed(token.c_str());
			proxy->close();
			return;
		}

		if (list->isActive())
		{
			QDBG("The file list has already been requested; closing");
			proxy->close();
			return;
		}

		QDBG("Requesting the file list of '%s'", list->getClientID());
		list->setActive(true);

		/*
		 * The whole share by name, rather than asked for as a list: a list
		 * request names one directory, and a peer that answers it properly
		 * sends only that directory's subdirectories, marked Incomplete="1"
		 * and empty, leaving nothing to browse into. Compressed if the peer
		 * says it holds a bzip2 encoded list, which "XmlBZList" is what it is
		 * called in $Supports; onFilelistDownloaded() unpacks whichever turns
		 * up.
		 *
		 * No ZL1: there is no inflating counterpart to ZUpload yet.
		 */
		send(new DC::SendADCGet("file",
			supports("XmlBZList") ? "files.xml.bz2" : "files.xml", 0, -1));
		return;
	}

	QuickDC::QueueFile* qfile = request->getFile();
	if (!qfile || !qfile->getTTH())
	{
		QERR("A queued file without a TTH cannot be requested with $ADCGET");
		queue->onRequestFailed(token.c_str());
		proxy->close();
		return;
	}

	/* Naming a file by its hash is the "TTHF" extension. */
	if (!supports("TTHF"))
	{
		QERR("'%s' cannot look files up by TTH", remote_nickname.c_str());
		queue->onRequestFailed(token.c_str());
		proxy->close();
		return;
	}

	uint64_t offset = 0;
	uint64_t length = 0;
	if (!queue->reserveChunk(qfile, request->getSource(), offset, length))
	{
		QDBG("Nothing left to fetch of '%s' from this peer", qfile->getLocalFilename());
		queue->onRequestFinished(token.c_str());
		proxy->close();
		return;
	}

	request->setReservation(offset, length);
	chunk_offset = offset;
	chunk_length = static_cast<int64_t>(length);

	char tthpath[64];
	snprintf(tthpath, sizeof(tthpath), "TTH/%s", qfile->getTTH());

	QDBG("Requesting %d bytes at offset %d of '%s'",
		static_cast<int>(length), static_cast<int>(offset), qfile->getLocalFilename());

	send(new DC::SendADCGet("file", tthpath, offset, static_cast<int64_t>(length)));
}


/**
 * We only keep track of our own direction, which is the opposite of the one
 * we will see here, provided that our priority is higher that is.
 */
void DC::ClientSession::onDirection(enum Direction dir, size_t prio)
{
	if (remote_direction != DirNone)
		return;

	remote_direction = dir;
	remote_prio = prio;
	QDBG("Remote user requested a %s with priority %zu", dir == DirDownload ? "download" : "upload", prio);
}


void DC::ClientSession::onNick(const char* nick)
{
	if (!remote_nickname.empty())
		return;

	remote_nickname = nick;

	if (!hubsession)
	{
		/*
		 * By nickname, which is what an NMDC peer is named by. lookupUser()
		 * would match a session ID instead on an ADC hub, so any four
		 * characters that happened to be someone's SID got in.
		 */
		QuickDC::Hub* hub = getCore().hubs->lookupUserByNick(nick);
		if (hub)
		{
			hubsession = hub->session;
		}
		else
		{
			QDBG("PANIC! Unable to lookup user from hub. The user does not exist. Ignore him!");
			proxy->close();
		}
	}
	STATECHANGE(StateNick, StateNone)
}


void DC::ClientSession::addSupport(const char* feature)
{
	if (feature) extensions.insert(feature);
}


bool DC::ClientSession::supports(const char* feature)
{
	return feature && extensions.count(feature) != 0;
}


/**
 * An error occured at the other party, should close
 * connection and retry later.
 */
void DC::ClientSession::onError(const char* msg)
{
	QDBG("Remote error: '%s'", msg);
	(void) msg;

	/* Typically "File Not Available"; the source cannot serve what we queued. */
	abandonDownload();
}


/**
 * We have requested a file, and this is it's filesize.
 */
void DC::ClientSession::onFileSize(uint64_t)
{

}

void DC::ClientSession::onTransferStopped(QuickDC::Transfer* which)
{
	// connection cancelled/closed
	QDBG("DC::ClientSession::onTransferStopped()");

	/*
	 * Only ever the transfer that stopped; see PrivateTransferListener. Releasing
	 * whatever this session happens to be holding would delete an object the socket
	 * is still feeding, and one we have moved on from is still ours to free.
	 */
	if (which && which != transfer)
	{
		QDBG("A transfer stopped that this connection has already moved on from");
		delete which;
		return;
	}

	state = StateIdle;

	if (!download_approved)
	{
		/* An upload finished; do nothing, just await a new request. */
		releaseTransfer();
		return;
	}

	QuickDC::Core* core = &getCore();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;

	/* Work out how it went before the transfer object goes away. */
	bool complete = (transfer && transfer->isFinished());
	uint64_t offset = chunk_offset;
	uint64_t length = (chunk_length > 0) ? static_cast<uint64_t>(chunk_length) : 0;

	QuickDC::PendingRequest* request = (queue && !token.empty()) ? queue->lookupRequest(token.c_str()) : 0;

	/* A file list is not chunked, so it either arrived whole or not at all. */
	QuickDC::FilelistRequest* list = 0;
	if (request && request->getKind() == QuickDC::PendingRequest::KindFilelist)
		list = request->getFilelist();

	releaseTransfer();

	if (queue == 0 || token.empty())
		return;

	if (list)
	{
		if (complete)
		{
			queue->onFilelistDownloaded(list, hubUrl(), getNick());
			queue->onRequestFinished(token.c_str());
			queue->removeFilelistRequest(list);
		}
		else
		{
			QDBG("The file list did not arrive in full");
			queue->onRequestFailed(token.c_str());
		}

		proxy->close();
		return;
	}

	if (!request)
		return;

	QuickDC::QueueFile* qfile = request->getFile();

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
			proxy->close();
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
	proxy->close();
}


/**
 * The other party does not have any slots available.
 * So the download will have to wait.
 */
void DC::ClientSession::onMaxedOut()
{
	QDBG("'%s' has no free slots", remote_nickname.c_str());

	/*
	 * Not the peer's fault and not a corrupt source, but there is nothing to
	 * be had here right now. Handing the chunk back frees it for another
	 * source; the retry count still climbs, which is what eventually stops us
	 * hammering a permanently full peer.
	 */
	abandonDownload();
	proxy->close();
}


/**
 * Return the length of MyList.DcLst -- this is only 
 * requested by legacy DC clients.
 */
void DC::ClientSession::onGetListLen()
{
	if (upload_approved && state == StateIdle)
	{
		QuickDC::Share::Manager* sharemgr = getCore().shares;
		file = sharemgr->getFile("MyList.DcLst");
		if (file)
		{
			send(new DC::SendListLength(file->size()));
		}
		else
		{
			sendErrorFileUnavailable();
		}
	}
}


/**
 * The other party has requested a file.
 * If length != -1 this means we have a CHUNK request, most likely
 * from a Valknut client, that means we will seek to that offset
 * and send that amount of data.
 */
void DC::ClientSession::onGet(const char* fn, uint64_t offset, int64_t chunklen)
{
	QDBG("Requested file (DC): '%s'", fn);
	if (upload_approved && state == StateIdle)
	{
		std::string filename(fn);
		std::replace(filename.begin(), filename.end(), '\\', '/');

		QDBG("looking up file: '%s'", filename.c_str());
		QuickDC::Share::Manager* sharemgr = getCore().shares;
		file = sharemgr->getFile(filename.c_str());

		if (file)
		{
			QDBG("Found file");
			
			chunk_offset = offset;
			chunk_length = chunklen;
			
			sendFileSize();
			
		}
		else
		{
			sendErrorFileUnavailable();
		}
	}
}


/**
 * The other party has requested a file.
 * If length == -1 we will send until end of file.
 */
void DC::ClientSession::onGetZBlock(const char* file, uint64_t offset, int64_t length)
{
	QDBG("Requested file (ZBLOCK): '%s', offset=%llu, length=%llu", file,
		static_cast<unsigned long long>(offset), static_cast<unsigned long long>(length));
	(void) file;
	(void) offset;
	(void) length;
	if (upload_approved && state == StateIdle)
	{
		
	}
}


#define FOURCC(a, b, c, d) (static_cast<uint32_t>( static_cast<uint32_t>(a) << 24 | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d)))

void DC::ClientSession::onADCGet(const char* fn, uint32_t type, uint64_t offset, int64_t length, bool zlib)
{
	QDBG("Requested file (ADCGET): '%s', offset=%d, length=%d", fn, static_cast<int>(offset), static_cast<int>(length));

	if (!upload_approved)
	{
		QDBG("Client requested a file to be sent, however no upload is approved. Ignoring request...");
		return;
	}

	if (zlib)
	{
		/* We never advertise ZLIG, so nothing should be asking. */
		QDBG("A compressed transfer was asked for, which we do not offer");
		sendErrorFileUnavailable();
		return;
	}

	/*
	 * One transfer at a time on a connection. A peer asking for a file while
	 * something is already moving had the running transfer released underneath it,
	 * which on a connection we were downloading over killed the download. Answered
	 * as busy instead, which is what it is.
	 */
	if (transfer)
	{
		QDBG("A transfer is already running on this connection; refusing");
		send(new DC::SendMaxedOut());
		return;
	}

	releaseTransfer();

	QuickDC::Core* core = &getCore();
	QuickDC::Share::Manager* sharemgr = core->shares;
	QuickDC::TransferManager* xfrmgr = core->transfers;

	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	int slots = config->getNumber("Slots", 3);
	bool slots_full = (slots > 0 && xfrmgr->countUploads() >= static_cast<size_t>(slots));

	uint64_t servable = 0;
	uint64_t internal_offset = 0;
	const char* kind = "file";

	if (type == FOURCC('l','i','s','t'))
	{
		kind = "list";

		/*
		 * Plain, whatever the peer says it can read: a list response carries
		 * the XML itself, and the compressed form is a file that a peer fetches
		 * by name instead. "BZList" is about the obsolete MyList.bz2 and says
		 * nothing about this.
		 */
		file = sharemgr->getFile("files.xml");
		slots_full = false;
	}
	else if (type == FOURCC('t','t','h','l') || type == FOURCC('t','t','h','1'))
	{
		kind = "tthl";
		if (strlen(fn) == 43 && FOURCC(fn[0],fn[1],fn[2],fn[3]) == FOURCC('T','T','H','/'))
		{
			/* Leaves for a shared file live inside the hash cache, which the
			   hash manager owns; ours are a file of their own. */
			Samurai::IO::File* shared = sharemgr->getFileByTTH(&fn[4]);
			if (shared)
			{
				/* The cache is the hash manager's, but the handle onto it is
				   made for this request and nobody else will close it. */
				file = core->hash->getLeafData(*shared, internal_offset, servable);
				if (file) ownsFile = true;
			}
			else
			{
				file = core->queue->openLeafFile(&fn[4], servable);
				if (file) ownsFile = true;
			}
		}
		slots_full = false;
	}
	else if (strlen(fn) == 43 && FOURCC(fn[0],fn[1],fn[2],fn[3]) == FOURCC('T','T','H','/'))
	{
		file = sharemgr->getFileByTTH(&fn[4]);

		if (!file)
		{
			file = core->queue->openVerifiedRange(&fn[4], offset, length, servable);
			if (file) ownsFile = true;
		}

		if (slots_full && file && file->size() < 65536)
			slots_full = false;

		/*
		 * And a bulletin board post whatever its size, up to the bound on how many
		 * of those may be in flight at once. A board is durable only because
		 * everyone who has read a post hands it on, so a reader turned away because
		 * somebody else is downloading a film is a board that stops working under
		 * exactly the load that makes it worth having. What keeps that affordable is
		 * the hub's own limit on how large a post may be, which is why these two
		 * numbers are chosen together.
		 */
		if (slots_full && file && sharemgr->mayServeWithoutSlot(&fn[4], xfrmgr))
			slots_full = false;
	}
	else
	{
		file = sharemgr->getFile(fn);
	}

	if (slots_full)
	{
		QDBG("Upload slot not available");
		send(new DC::SendMaxedOut());
		return;
	}

	if (!file)
	{
		sendErrorFileUnavailable();
		return;
	}

	if (!servable) servable = static_cast<uint64_t>(file->size());

	chunk_offset = offset;
	if (length < 0 || offset + static_cast<uint64_t>(length) > servable)
		chunk_length = static_cast<int64_t>(servable - offset);
	else
		chunk_length = length;

	if (offset > servable || chunk_length <= 0)
	{
		QDBG("Asked for %d of '%s', which is not ours to send", static_cast<int>(offset), fn);
		sendErrorFileUnavailable();
		return;
	}

	send(new DC::SendADCSnd(kind, (type == FOURCC('l','i','s','t')) ? "/" : fn,
		chunk_offset, static_cast<uint64_t>(chunk_length)));

	transfer = new QuickDC::Upload(xfrmgr, this, proxy->socket, file,
		internal_offset + chunk_offset, chunk_length);
}


void DC::ClientSession::onADCSend(const char* filename, uint32_t type, uint64_t offset, int64_t length, bool compressed)
{
	QDBG("Receiving file (ADCSND): '%s', offset=%d, length=%d", filename, static_cast<int>(offset), static_cast<int>(length));
	(void) filename;

	QuickDC::Core* core = &getCore();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	QuickDC::TransferManager* xfrmgr = (core) ? core->transfers : 0;

	if (!download_approved || !queue || !xfrmgr || token.empty())
	{
		QERR("Unexpected $ADCSND; no download is in progress on this connection");
		return;
	}

	if (compressed)
	{
		/* We never ask for ZL1, and cannot inflate a stream that arrives anyway. */
		QERR("The peer sent a compressed stream, which is not supported");
		queue->onRequestFailed(token.c_str());
		proxy->close();
		return;
	}

	QuickDC::PendingRequest* request = queue->lookupRequest(token.c_str());
	if (!request)
	{
		QERR("$ADCSND for a token we no longer know about ('%s')", token.c_str());
		proxy->close();
		return;
	}

	if (length <= 0)
	{
		QERR("The peer offered a nonsensical length (%d)", static_cast<int>(length));
		queue->onRequestFailed(token.c_str());
		proxy->close();
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
			proxy->close();
			return;
		}

		/* Start from scratch; a list is not resumable. */
		Samurai::IO::File(list->getTempFilename()).remove();

		file = new Samurai::IO::File(list->getTempFilename());
		ownsFile = true;

		chunk_offset = 0;
		chunk_length = length;
		request->setReservation(0, static_cast<uint64_t>(length));

		transfer = new QuickDC::Download(xfrmgr, this, socket, file, 0, length);
		state = StateTransfer;
		return;
	}

	if (type == FOURCC('t','t','h','1'))
	{
		/* Leaf hashes are not requested yet; see the download queue notes. */
		QDBG("Ignoring an unsolicited tthl response");
		return;
	}

	/* type == file */
	QuickDC::QueueFile* qfile = request->getFile();
	if (!qfile)
	{
		QERR("The peer sent file data we did not ask for");
		queue->onRequestFailed(token.c_str());
		proxy->close();
		return;
	}

	/*
	 * A peer may answer with less than we asked for, but not with something
	 * outside the chunk we reserved - that would corrupt ranges already
	 * accounted for, or that another source is fetching.
	 */
	if (!request->haveReservation()
		|| offset != request->getReservedOffset()
		|| static_cast<uint64_t>(length) > request->getReservedLength())
	{
		QERR("The peer answered with %d bytes at %d, which is not the chunk we reserved",
			static_cast<int>(length), static_cast<int>(offset));
		queue->onRequestFailed(token.c_str());
		proxy->close();
		return;
	}

	file = new Samurai::IO::File(qfile->getTempFilename());
	ownsFile = true;

	chunk_offset = offset;
	chunk_length = length;

	/* Hold the request to what the peer actually promised. */
	request->setReservation(offset, static_cast<uint64_t>(length));

	transfer = new QuickDC::Download(xfrmgr, this, socket, file, offset, length);
	state = StateTransfer;
}


/**
 * The other party has asked us to start transmitting.
 */
void DC::ClientSession::onSend()
{
	if (upload_approved && file)
	{
		/*
		 * The transfer only, not the file: this is a second $Send for the file the
		 * last $Get set up, so the file has to stay open while whatever was running
		 * on it goes.
		 */
		delete transfer;
		transfer = 0;

		QuickDC::TransferManager* manager = getCore().transfers;
		transfer = new QuickDC::Upload(manager, this, proxy->socket, file, chunk_offset, chunk_length);
	}
	else
	{
		QDBG("Client requested a file to be sent, however no file is selected yet. Silently ignoring request...");
		// proxy->close();
	}
}


void DC::ClientSession::EventConnected(const Samurai::IO::Net::Socket*)
{
	QDBG("Connected to peer");
	onStartClient();
	manager->add(this);
}


void DC::ClientSession::EventTimeout(const Samurai::IO::Net::Socket*)
{
	QDBG("Peer timeout");
	abandonDownload();
	manager->remove(this);
}


void DC::ClientSession::EventDisconnected(const Samurai::IO::Net::Socket*)
{
	QDBG("Disconnected from peer");
	abandonDownload();

	/* The command proxy shares the socket, so close the descriptor rather than
	   just letting go of one of the two references to it. */
	socket->disconnect();
	socket.reset();
	manager->remove(this);
}


void DC::ClientSession::EventDataAvailable(const Samurai::IO::Net::Socket*)
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


void DC::ClientSession::EventCanWrite(const Samurai::IO::Net::Socket*)
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


void DC::ClientSession::EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char*)
{
	abandonDownload();

	/* The command proxy shares the socket, so close the descriptor rather than
	   just letting go of one of the two references to it. */
	socket->disconnect();
	socket.reset();
	manager->remove(this);
}

