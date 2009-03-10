/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
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
#include "network/connectionmanager.h"
#include "network/hubmanager.h"
#include "network/transfer.h"
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

DC::ClientSession::ClientSession(Samurai::IO::Net::URL* url, DC::HubSession* hubsession_) :
	QuickDC::Connection(url, "NMDC client"),
	hubsession(hubsession_),
	isClient(true)
{
	initialize();
	proxy = new DC::CommandProxy(socket, this);
	QDBG("DC::ClientSession()");
}


DC::ClientSession::ClientSession(Samurai::IO::Net::Socket* sock, DC::HubSession* hubsession_) :
	QuickDC::Connection(sock, "NMDC client"),
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
	while (extensions.size())
	{
		char* feature = *extensions.begin();
		extensions.erase(extensions.begin());
		free(feature);
	}
	
	delete transfer;
	transfer = 0;
	
	free(remote_nickname);
	free(remote_lock);
	free(remote_key);
	free(remote_pk);
	free(my_key);
	
	delete proxy;

	QDBG("~DC::ClientSession()");
}


void DC::ClientSession::initialize()
{
	remote_nickname  = 0;
	remote_lock      = 0;
	remote_pk        = 0;
	remote_key       = 0;
	my_key           = 0;
	my_prio          = 0;
	remote_prio      = 0;
	proxy            = 0;
	transfer         = 0;
	file             = 0;
	chunk_offset     = 0;
	chunk_length     = 0;
	upload_approved  = false;
	extendedProtocol = false;
	state            = StateNone;
	remote_direction = DirNone;
	my_direction     = DirNone;
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

	char* my_lock = Lock::createLock();
	char* my_pk = Lock::createPk();
	my_key = DC::Lock::calculateKey(my_lock, strlen(my_lock));
	
	send(new DC::SendMyNick(hubsession->getLocalUser()->getNick()), true);
	send(new DC::SendLock(my_lock, my_pk));

	free(my_lock);
	free(my_pk);

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
		// FIXME: Check if we want download
		my_direction = DirUpload;
		my_prio = Samurai::Util::pseudoRandom(1000, 32767);

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
		send(new DC::SendKey(remote_key, strlen(remote_key)));
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
	if (remote_lock || remote_pk) return;

	remote_lock = strdup(lock_);
	remote_pk = strdup(pk_);
	
	extendedProtocol = (strncmp(remote_lock, "EXTENDEDPROTOCOL", 16) == 0);
	remote_key = Lock::calculateKey(remote_lock, strlen(remote_lock));
	
	if (!isClient)
	{
		sendChallenge();
		STATECHANGE(StateLock, StateNick);
		sendDirection();
		sendKey();
	}
}


// FIXME: Check key here
void DC::ClientSession::onKey(const char* /* key_ */)
{
	if (isClient)
	{
		sendDirection();
		sendKey();
	}
	
	// NOTE: If we won the direction negotiation, we will now proceed driving the
	//       protocol, and the other party is serving us.
	if (my_direction == remote_direction && my_direction == DirDownload && my_prio > remote_prio)
	{
		QDBG("Download approved by other party.");
		upload_approved = false;
		// FIXME: Start requesting data
	}
	else
	{
		QDBG("Upload approved.");
		upload_approved = true;
	}
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
	QDBG("Remote user requested a %s with priority %d", dir == DirDownload ? "download" : "upload", prio);
}


void DC::ClientSession::onNick(const char* nick)
{
	if (remote_nickname)
		return;

	remote_nickname = strdup(nick);
	
	if (!hubsession)
	{
		QuickDC::Hub* hub = QuickDC::Core::getInstance()->hubs->lookupUser(nick);
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
	extensions.insert(strdup(feature));
}


// FIXME: does this work?
bool DC::ClientSession::supports(const char* feature)
{
	char* f = strdup(feature);
	bool found = extensions.count(f);
	free(f);
	return found;
}


/**
 * An error occured at the other party, should close
 * connection and retry later.
 */
void DC::ClientSession::onError(const char* msg)
{
	QDBG("Remote error: '%s'", msg);
}


/**
 * We have requested a file, and this is it's filesize.
 */
void DC::ClientSession::onFileSize(uint64_t)
{

}

void DC::ClientSession::onTransferStopped()
{
	// connection cancelled/closed
	QDBG("DC::ClientSession::onTransferStopped()");
	
	delete transfer; transfer = 0;
	file = 0; // NOTE: NEVER DELETE!
	state = StateIdle;
	chunk_offset = 0;
	chunk_length = -1;

	
	if (upload_approved)
	{
		/* do nothing, just await new request */
	}
	else
	{
		// Fetch next file from queue.
	}
}


/**
 * The other party does not have any slots available.
 * So the download will have to wait.
 */
void DC::ClientSession::onMaxedOut()
{

}


/**
 * Return the length of MyList.DcLst -- this is only 
 * requested by legacy DC clients.
 */
void DC::ClientSession::onGetListLen()
{
	if (upload_approved && state == StateIdle)
	{
		QuickDC::Share::Manager* sharemgr = QuickDC::Core::getInstance()->shares;
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
		char* filename = strdup(fn);
		for (size_t i = 0; i < strlen(filename); i++)
			if (filename[i] == '\\') filename[i] = '/';
		
		QDBG("looking up file: '%s'", filename);
		QuickDC::Share::Manager* sharemgr = QuickDC::Core::getInstance()->shares;
		file = sharemgr->getFile(filename);
		free(filename);
		
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
	QDBG("Requested file (ZBLOCK): '%s', offset=%u, length=%l", file, offset, length);
	if (upload_approved && state == StateIdle)
	{
		
	}
}


#define FOURCC(a, b, c, d) ((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d))

void DC::ClientSession::onADCGet(const char* fn, uint32_t type, uint64_t offset, int64_t length, bool zlib)
{
	(void) type;
	(void) zlib;
	
	QDBG("Requested file (ADCGET): '%s', offset=%u, length=%l", fn, offset, length);
	
	
#if 0
	if (!upload_approved || state != StateIdle)
	{
		QDBG("Client requested a file to be sent, however no upload is approved. Ignoring request...");
		return;
	}
	file = 0;
	bool tth_file = false;
	
	QuickDC::Share::Manager* sharemgr = QuickDC::Core::getInstance()->shares;

	if (type == FOURCC('l','i','s','t')) {
		file = sharemgr->getFile(supports("BZList") ? "files.xml.bz2" : "files.xml");
	}
	else if (type == FOURCC('t','t','h','1'))
	{
			/* Transfer merkle tree */
	}
	else
	{ /* type == file */
		if (strlen(fn) == 43 && FOURCC(fn[0],fn[1],fn[2],fn[3]) == FOURCC('T','T','H','/'))
		{
			
			file = sharemgr->getFileByTTH(&fn[4]);
			tth_file = true;
		}
		else
		{
			file = sharemgr->getFile(fn);
		}
	}
	
	if (file)
	{
		QDBG("Found file");
		
		chunk_offset = offset;
		chunk_length = length;
		
		proxy->write("$ADCSND ");
		
		if (type == FOURCC('l','i','s','t'))
		{
			proxy->write("list ");
			proxy->write("/ ");
		}
		else if (type == FOURCC('t','t','h','1'))
		{
			proxy->write("tth1 ");
			proxy->write(fn);
			proxy->write(' ');
		}
		else
		{
			proxy->write("file ");
			proxy->write(fn);
			proxy->write(' ');
		}
		
		proxy->write(chunk_offset);
		proxy->write(' ');
		if (chunk_length == -1)
			proxy->write("-1");
		else
			proxy->write((uint64_t) chunk_length);

#if 0 /* Only if we support it */
		if (zlib) {
			proxy->write(" ZL1");
		}
#endif
		proxy->write('|');
		proxy->write();

		transfer = new QuickDC::Upload(QuickDC::Core::getInstance()->transfers, this, proxy->socket, file, chunk_offset, chunk_length);
		
		sendFileSize();
			
	}
	else
	{
		sendErrorFileUnavailable();
	}
#endif
}


void DC::ClientSession::onADCSend(const char* file, uint32_t type, uint64_t offset, int64_t length, bool)
{
	QDBG("Receiving file (ADCSND): '%s', offset=%u, length=%l", file, offset, length);
	
	if (type == FOURCC('l','i','s','t'))
	{
		
	}
	else if (type == FOURCC('t','t','h','1'))
	{
		
	}
	else
	{
		
	}
}


/**
 * The other party has asked us to start transmitting.
 */
void DC::ClientSession::onSend()
{
	if (upload_approved && file)
	{
		QuickDC::TransferManager* manager = QuickDC::Core::getInstance()->transfers;
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
	manager->remove(this);
}


void DC::ClientSession::EventDisconnected(const Samurai::IO::Net::Socket*)
{
	QDBG("Disconnected from peer");
	delete socket;
	socket = 0;
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
	delete socket;
	socket = 0;
	manager->remove(this);
}

