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
		ClientSession(QuickDC::Core& core, std::shared_ptr<Samurai::IO::Net::Socket> sock, DC::HubSession* hubsession = 0);
		ClientSession(QuickDC::Core& core, Samurai::IO::Net::URL* url, DC::HubSession* hubsession = 0);
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

		void onStartServer() override;
		void onStartClient() override;
		
		void sendChallenge();
		void sendDirection();
		void sendKey();
		void sendErrorFileUnavailable();
		void sendFileSize();

		/**
		 * Ask the peer for the next thing the download queue wants from it,
		 * and drop the connection when there is nothing left to ask for.
		 *
		 * Only meaningful once we have won the direction negotiation.
		 */
		void requestNextChunk();

		void addSupport(const char* features);
		bool supports(const char* feature);
		bool isExtendedProtocol() const { return extendedProtocol; }
		
		/**
		 * @return the nickname the peer presented in $MyNick, or null before it
		 *         has presented one. An unnamed peer is what an inbound
		 *         connection is until it speaks.
		 */
		const char* getNick() const
		{ return remote_nickname.empty() ? 0 : remote_nickname.c_str(); }
		
	protected:
		void EventConnected(const Samurai::IO::Net::Socket*) override;
		void EventTimeout(const Samurai::IO::Net::Socket*) override;
		void EventDisconnected(const Samurai::IO::Net::Socket*) override;
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override;
		void EventCanWrite(const Samurai::IO::Net::Socket*) override;
		void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char* msg) override;

	protected:
		void send(DC::Command* cmd, bool more = false);
		void resetIdleTimer();
		
	public:
		void onTransferStopped(QuickDC::Transfer* which) override;
		
	protected:
		/*
		 * Each of these is empty until the peer, or the challenge we send it,
		 * has supplied it. None of them has an empty value that means anything,
		 * so the two cases do not need telling apart.
		 */
		std::string remote_nickname;
		std::string remote_lock;
		std::string remote_pk;
		std::string remote_key;
		std::string my_key;
		size_t my_prio;
		size_t remote_prio;
		enum Direction remote_direction;
		enum Direction my_direction;
		bool extendedProtocol;
		/* Held by value, so that a lookup compares the name and not a pointer. */
		std::set<std::string> extensions;
		enum State state;
		DC::CommandProxy* proxy;
		QuickDC::HubSession* hubsession;
		
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

		/**
		 * Whether the download queue wants anything from this user, which is
		 * what decides the direction we ask for.
		 */
		bool wantToDownload();

		QuickDC::Transfer* transfer;
		Samurai::IO::File* file;

		/**
		 * Uploads hand out a file owned by the share manager, downloads use
		 * one we made ourselves. Only the latter may be deleted.
		 */
		bool ownsFile;

		uint64_t chunk_offset;
		int64_t chunk_length;

		/**
		 * Which queue job this connection is servicing.
		 *
		 * NMDC has nowhere to put a token on the wire, so unlike ADC this is
		 * not exchanged with the peer - it is looked up from the queue by which
		 * user we are talking to, and then kept for the rest of the session.
		 * Empty until there is a job.
		 */
		std::string token;

	private:
		bool challenged;
		bool isClient;
		bool upload_approved;
		bool download_approved;
		
		friend class QuickDC::Connection;
};

}

#endif // HAVE_QUICKDC_DCCLIENTSESSION_H
