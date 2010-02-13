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
#include "network/adc/adccommandproxy.h"
#include "network/adc/adccommands.h"
#include "network/adc/parser.h"
#include "network/adc/cid.h"
#include <samurai/io/file.h>

ADC::ClientSession::ClientSession(Samurai::IO::Net::URL* url, ADC::HubSession* hubsession_) :
	QuickDC::Connection(url, "ADC client"),
	hubsession(hubsession_),
	isClient(true)
{
	initialize();
	proxy = new ADC::CommandProxy(socket, this);
	QDBG("ADC::ClientSession()");
}

ADC::ClientSession::ClientSession(Samurai::IO::Net::Socket* sock, ADC::HubSession* hubsession_, const char* token_, const char* sid_) :
	QuickDC::Connection(sock, "ADC client"),
	hubsession(hubsession_),
	isClient(false)
{
	if (token_) token = strdup(token_);
	else token = 0;
	
	if (sid_) remote_sid = strdup(sid_);
	else remote_sid = 0;
	
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
	
	delete transfer; transfer = 0;
	delete proxy;
	QDBG("~ADC::ClientSession()");
	
	delete socket; socket = 0;
	
	free(token);
	free(remote_sid);
}

void ADC::ClientSession::initialize()
{
	proxy            = 0;
	transfer         = 0;
	file             = 0;
	chunk_offset     = 0;
	chunk_length     = 0;
	upload_approved  = false;
	state            = StateNone;
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
	
	if (token) {
		upload_approved = QuickDC::Core::getInstance()->securityManager->isAuthorized(QuickDC::Clearance::ADC, hubsession, socket, cid, token);
		QDBG("Authorized for upload: %s", upload_approved ? "yes" : "no");
	} else if (remote_token) {
		QDBG("FIXME: We have a remote token. So lets lookup what to do about it");
		upload_approved = true;
	} else {
		QDBG("FIXME: nobody wants to do anything! - Close connection");
	}
		
	if (isClient)
	{
		sendINF();
	}
	else
	{
		/* request download */
	}
}

void ADC::ClientSession::onGFI(const char*, uint32_t)
{
	if (upload_approved)
	{
		
	}
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
	QDBG("DC::ClientSession::onTransferStopped()");
	
	delete transfer; transfer = 0;
	file = 0; // NOTE: NEVER DELETE!
	chunk_offset = 0;
	chunk_length = -1;

	if (upload_approved) {
		/* do nothing, just await new request */
	} else {
		// Fetch next file from queue.
	}
}


#define FOURCC(a, b, c, d) ((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d))

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
	bool slots_full = false;  // FIXME: Check if slots are available
	
	QuickDC::Share::Manager* sharemgr = QuickDC::Core::getInstance()->shares;
	QuickDC::Hash::Manager*   hashmgr = QuickDC::Core::getInstance()->hash;
	QuickDC::TransferManager*  xfrmgr = QuickDC::Core::getInstance()->transfers;
	
	if (type == FOURCC('l','i','s','t'))
	{
		QDBG("looking up files.xml");
		file = sharemgr->getFile(supports(ADC_EXT_ZLIG) ? ADC_INDEX_FILE_BZ2 : ADC_INDEX_FILE);
		if (file) internal_length = file->size();
		slots_full = false;
	}
	else if (type == FOURCC('t','t','h','l'))
	{
		/* Transfer merkle tree */
		QDBG("looking up merkle tree for file: %s", fn);
		if (strlen(fn) == 43 && FOURCC(fn[0],fn[1],fn[2],fn[3]) == FOURCC('T','T','H','/'))
		{
			Samurai::IO::File* tthfile = sharemgr->getFileByTTH(&fn[4]);
			
			QuickDC::Hash::CacheEntry* hashCache = 0;
			if (hashmgr->lookup(*tthfile, hashCache))
			{
				if (offset > (uint64_t) hashCache->getCacheFileOffset())
				{
					QDBG("Invalid file offset");
				}
				else
				{
					file = hashmgr->getCacheFile();
					QDBG("Found file in hash cache. Offset=%d  filesize=%d:-)", (int) hashCache->getCacheFileOffset(), (int) hashCache->getSize() );
					internal_length = hashCache->getLeafdataSize();
					internal_offset = hashCache->getCacheFileOffset();
				}
			}
			else
			{
				QDBG("Unable to lookup file");
			}
		}
		
	}
	else if (type == FOURCC('f','i','l','e'))
	{
		QDBG("looking up file: %s", fn);
		if (strlen(fn) == 43 && FOURCC(fn[0],fn[1],fn[2],fn[3]) == FOURCC('T','T','H','/'))
		{
			file = sharemgr->getFileByTTH(&fn[4]);
			
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
	
		if (file) internal_length = file->size();
		
	} else {
		QDBG("Unexpected GET type: %x type, expected %x or %x.", type,  FOURCC('l','i','s','t'),  FOURCC('f','i','l','e'));
	}
	
	if (slots_full)
	{
		QDBG("Upload slot not available\n");
		ADC::Command* cmd = new ADC::Command(FOURCC('C','S','T','A'));
		cmd->addArgument(quickdc_itoa(ADC_STATUS_SEVERITY_ERROR, 10), quickdc_itoa(ADC_CODE_TRANSFER_SLOT_ERROR, 10));
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
		cmd->addArgument(quickdc_itoa(ADC_STATUS_SEVERITY_ERROR, 10), quickdc_itoa(ADC_CODE_TRANSFER_FILE_ERROR, 10));
		cmd->addArgument(ADC_STR_TRANSFER_FILE_ERROR);
		send(cmd);
	}
}

void ADC::ClientSession::onSND(const char* file, uint32_t type, uint64_t offset, int64_t length, bool)
{
	QDBG("Receiving file (ADCSND): '%s', offset=%u, length=%l", file, offset, length);
	
	if (type == FOURCC('l','i','s','t')) {
		
	} else if (type == FOURCC('t','t','h','l')) {
		
	} else { /* type == file */
		
	}
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
	onStartClient();
}

void ADC::ClientSession::EventTimeout(const Samurai::IO::Net::Socket*)
{
	QDBG("Peer timeout");
	manager->remove(this);
}

void ADC::ClientSession::EventDisconnected(const Samurai::IO::Net::Socket*)
{
	QDBG("Disconnected from peer");
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
	manager->remove(this);
}


