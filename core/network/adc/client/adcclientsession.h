/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCCLIENTSESSION_H
#define HAVE_QUICKDC_ADCCLIENTSESSION_H

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

namespace ADC {
class Command;
class CommandProxy;
class HubSession;

bool probe(const char* handshake, size_t length);

/**
 * A class describing a client connection's state.
 */
class ClientSession :
		public QuickDC::ClientSession,
		public QuickDC::Connection,
		public QuickDC::PrivateTransferListener
{
	public:
		
		/* ADC protocol states */
		enum State {
			StateNone,
			StateProtocol,
			StateIdentify,
			StateVerify,
			StateNormal,
			StateData,
		};

		/**
		 * NOTE: hubsession may be 0. It always is if we don't know who we are talking to, yet.
		 */
		ClientSession(Samurai::IO::Net::Socket* sock, ADC::HubSession* hubsession = 0, const char* token = 0, const char* sid = 0);
		ClientSession(Samurai::IO::Net::URL* url, ADC::HubSession* hubsession = 0);
		virtual ~ClientSession();
		
		void setHubSession(QuickDC::HubSession* hubsession) { this->hubsession = hubsession; }
		void initialize();
		
		void onStartServer();
		void onStartClient();
		
		void onINF(const char* cid, const char* token);
		
		void onRES();
		void onSUP();
		void onGET(const char* fn, uint32_t type, uint64_t offset, int64_t length, bool);
		void onSND(const char* file, uint32_t type, uint64_t offset, int64_t length, bool);
		void onGFI(const char* file, uint32_t type);

		void sendINF();

		void addSupport(const char* features);
		bool supports(const char* feature);
		
	protected:
		void EventHostLookup(const Samurai::IO::Net::Socket*);
		void EventHostFound(const Samurai::IO::Net::Socket*);
		void EventConnecting(const Samurai::IO::Net::Socket*);
		void EventConnected(const Samurai::IO::Net::Socket*);
		void EventTimeout(const Samurai::IO::Net::Socket*);
		void EventDisconnected(const Samurai::IO::Net::Socket*);
		void EventDataAvailable(const Samurai::IO::Net::Socket*);
		void EventCanWrite(const Samurai::IO::Net::Socket*);
		void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char* msg);

		void send(ADC::Command* cmd, bool more = false);

	public:
		void onTransferStopped();
		
	protected:
		std::set<char*> extensions;
		enum State state;
		ADC::CommandProxy* proxy;
		QuickDC::HubSession* hubsession;
		
		char* remote_sid;
		char* token;
		
	protected: /* transfer related */
		QuickDC::Transfer* transfer;
		Samurai::IO::File* file;
		uint64_t chunk_offset;
		int64_t chunk_length;
		
	private:
		bool isClient;
		bool upload_approved;
		friend class QuickDC::Connection;
};

}

#endif // HAVE_QUICKDC_ADCCLIENTSESSION_H
