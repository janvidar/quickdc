/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCCLIENTSESSION_H
#define HAVE_QUICKDC_DCCLIENTSESSION_H

#include "quickdc.h"
#include <string.h>
#include <set>
#include <string>
#include "network/clientsession.h"
#include "network/connection.h"
#include <samurai/io/net/socketevent.h>
#include "network/transfer.h"

namespace Samurai {
	namespace IO {
		class File;
		namespace Net {
			class Socket;
		}
	}
}

namespace QuickDC {
class HubSession;
class Transfer;
}

namespace DC {
class ClientSession;
class Command;
class CommandProxy;
class HubSession;

/**
 * A class describing a client connection's state.
 */
class ClientSession :
		public QuickDC::ClientSession,
		public QuickDC::Connection,
		public QuickDC::PrivateTransferListener
{
	public:
		enum Direction {
			DirNone,
			DirUpload,
			DirDownload
		};
		
		/* DC protocol states */
		enum State {
			StateNone,
			StateConnected,
			StateNick,
			StateLock,
			StateDirection,
			StateIdle,
			StateTransfer,
			StateDisconnected
		};

		/**
		 * NOTE: hubsession may be 0. It always is if we don't know who we are talking to, yet.
		 */
		ClientSession(Samurai::IO::Net::Socket* sock, DC::HubSession* hubsession = 0);
		ClientSession(Samurai::IO::Net::URL* url, DC::HubSession* hubsession = 0);
		virtual ~ClientSession();
		
		void setHubSession(QuickDC::HubSession* hubsession) { this->hubsession = hubsession; }
		void initialize();
		
		/**
		 * Add support for a feature a server supports
		 */
		void onLock(const char* lock, const char* pk);
		void onKey(const char* key);
		void onNick(const char* nick);
		void onDirection(enum Direction dir, size_t prio);
		void onError(const char* message);
		void onFileSize(uint64_t size);
		void onMaxedOut();
		void onGetListLen();
		void onGet(const char* filename, uint64_t offset, int64_t length);
		void onGetZBlock(const char* filename, uint64_t offset, int64_t length);
		void onADCGet(const char* filename, uint32_t type, uint64_t offset, int64_t length, bool zlib);
		void onADCSend(const char* filename, uint32_t type, uint64_t offset, int64_t length, bool zlib);
		void onSend();

		void onStartServer();
		void onStartClient();
		
		void sendChallenge();
		void sendDirection();
		void sendKey();
		void sendErrorFileUnavailable();
		void sendFileSize();
		
		void addSupport(const char* features);
		bool supports(const char* feature);
		bool isExtendedProtocol() const { return extendedProtocol; }
		
		const char* getNick() const { return remote_nickname; }
		
	protected:
		void EventConnected(const Samurai::IO::Net::Socket*);
		void EventTimeout(const Samurai::IO::Net::Socket*);
		void EventDisconnected(const Samurai::IO::Net::Socket*);
		void EventDataAvailable(const Samurai::IO::Net::Socket*);
		void EventCanWrite(const Samurai::IO::Net::Socket*);
		void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char* msg);

	protected:
		void send(DC::Command* cmd, bool more = false);
		void resetIdleTimer();
		
	public:
		void onTransferStopped();
		
	protected:
		char* remote_nickname;
		char* remote_lock;
		char* remote_pk;
		char* remote_key;
		char* my_key;
		size_t my_prio;
		size_t remote_prio;
		enum Direction remote_direction;
		enum Direction my_direction;
		bool extendedProtocol;
		std::set<char*> extensions;
		enum State state;
		DC::CommandProxy* proxy;
		QuickDC::HubSession* hubsession;
		
	protected: /* transfer related */
		QuickDC::Transfer* transfer;
		Samurai::IO::File* file;
		uint64_t chunk_offset;
		int64_t chunk_length;
		
	private:
		bool challenged;
		bool isClient;
		bool upload_approved;
		
		friend class QuickDC::Connection;
};

}

#endif // HAVE_QUICKDC_DCCLIENTSESSION_H
