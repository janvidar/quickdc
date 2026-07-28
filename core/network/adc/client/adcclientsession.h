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
class FilelistRequest;
class HubSession;
class QueueFile;
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
		 *
		 * @param secure this connection carries ADC inside TLS. When we are
		 *               dialling out, the handshake is started from here;
		 *               probe.cpp has already completed one by the time it
		 *               hands an inbound connection over.
		 * @param keyprint the certificate the peer is expected to present, from
		 *               the KP field of their INF. Only pass one when the hub
		 *               that relayed it was itself verified; the connection is
		 *               dropped if it does not match.
		 */
		ClientSession(Samurai::IO::Net::Socket* sock, ADC::HubSession* hubsession = 0, const char* token = 0, const char* sid = 0, bool secure = false, const char* keyprint = 0);
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

		/** A queued file's cached leaf hashes, for upload. Caller owns it. */
		Samurai::IO::File* openQueuedLeaves(const char* tth, uint64_t& length);

		/**
		 * A partially downloaded file, if the range asked for is one we have
		 * verified. Caller owns what is returned.
		 */
		Samurai::IO::File* openQueuedFile(const char* tth, uint64_t offset, int64_t length, uint64_t& servable);

		/** The peer refused, or reported a problem with, what we asked for. */
		void onSTA(const char* code, const char* message);

		void sendINF();

		/**
		 * Ask the peer for the next thing the download queue wants from it,
		 * and drop the connection when there is nothing left to ask for.
		 *
		 * Safe to call repeatedly; each call issues at most one CGET.
		 */
		void requestNextChunk();

		/** True while the current transfer is leaf hashes rather than file data. */
		bool fetchingLeaves;

		void addSupport(const char* features);
		bool supports(const char* feature);

	protected:
		void EventHostLookup(const Samurai::IO::Net::Socket*);
		void EventHostFound(const Samurai::IO::Net::Socket*);
		void EventConnecting(const Samurai::IO::Net::Socket*);
		void EventConnected(const Samurai::IO::Net::Socket*);
		void EventTLSConnected(const Samurai::IO::Net::Socket*);
		void EventTLSDisconnected(const Samurai::IO::Net::Socket*);
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

		/**
		 * The peer's CID, learned from their CINF. Unlike the SID this is
		 * stable across sessions, so it is what a FileSource records.
		 */
		char* remote_cid;

		char* token;

		/**
		 * The keyprint the peer's certificate has to match, or 0 if there is
		 * nothing trustworthy to check it against.
		 */
		char* expected_keyprint;

	protected: /* transfer related */

		/** Tear down the current transfer, and the file if we own it. */
		void releaseTransfer();

		/**
		 * Hand an unfinished chunk back to the queue, so that losing a peer
		 * does not block that part of the file for the rest of the run.
		 */
		void abandonDownload();

		/** The canonical URL of the hub this peer was found on, or 0. */
		const char* hubUrl() const;

		QuickDC::Transfer* transfer;
		Samurai::IO::File* file;

		/**
		 * Uploads hand out a file owned by the share manager, downloads use
		 * one we made ourselves. Only the latter may be deleted.
		 */
		bool ownsFile;

		uint64_t chunk_offset;
		int64_t chunk_length;

	private:
		bool isClient;

		/** The peer wants ADCS, so TLS has to come up before any ADC. */
		bool secure;

		bool upload_approved;
		bool download_approved;
		friend class QuickDC::Connection;
};

}

#endif // HAVE_QUICKDC_ADCCLIENTSESSION_H
