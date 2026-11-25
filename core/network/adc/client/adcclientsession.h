/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCCLIENTSESSION_H
#define HAVE_QUICKDC_ADCCLIENTSESSION_H

#include "quickdc.h"
#include <string.h>
#include <optional>
#include <set>
#include <string>
#include "network/clientsession.h"
#include "network/connection.h"
#include "network/adc/adccommon.h"
#include <samurai/io/net/socketevent.h>
#include <samurai/timer.h>
#include "network/transfer.h"
#include <chrono>
#include <memory>

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
		public QuickDC::PrivateTransferListener,
		public Samurai::TimerListener
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
		ClientSession(QuickDC::Core& core, std::shared_ptr<Samurai::IO::Net::Socket> sock, ADC::HubSession* hubsession = 0, const char* token = 0, const char* sid = 0, bool secure = false, const char* keyprint = 0);
		ClientSession(QuickDC::Core& core, Samurai::IO::Net::URL* url, ADC::HubSession* hubsession = 0);
		virtual ~ClientSession();
		
		void setHubSession(QuickDC::HubSession* hubsession) { this->hubsession = hubsession; }
		void initialize();
		
		void onStartServer() override;
		void onStartClient() override;
		
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

		/** Refuse what the peer asked for, there being nothing here to send. */
		void sendFileError();

		/**
		 * Ask the peer for the next thing the download queue wants from it,
		 * and drop the connection when there is nothing left to ask for.
		 *
		 * Safe to call repeatedly; each call issues at most one CGET.
		 */
		void requestNextChunk();

		/** True while the current transfer is leaf hashes rather than file data. */
		bool fetchingLeaves;

		/*
		 * Where this connection writes the leaves it is fetching. Its own, since
		 * another connection may be fetching the same file's; the first to arrive
		 * whole becomes the file's.
		 */
		std::string leafPart;
		void discardLeafPart();

		void addSupport(const char* features);
		bool supports(const char* feature);

		/** Whether the handshake is done and the peer has named itself. */
		bool isEstablished() const;

		/**
		 * Close the connection unless both sides have named themselves within this
		 * long from now. Every connection is given QUICKDC_PEER_HANDSHAKE_SECONDS
		 * from when it is up, so a peer that dials and says nothing holds no socket.
		 */
		void setHandshakeDeadline(std::chrono::milliseconds within);

		/**
		 * Make this a NAT traversal dial to @p host:@p port from local port
		 * @p from, which the peer is making at the same moment, playing the end
		 * @p dial says (SentNat or SentRnt). Before the socket connects.
		 *
		 * A refusal is dialled again: it means our SYN reached the peer's host
		 * before its own dial existed, through a NAT that still remembered an
		 * earlier connection between the same two ports. A dial that has not
		 * connected within QUICKDC_TRAVERSAL_SECONDS, or fails otherwise before
		 * both sides have named themselves, is reported on the hub and fails the
		 * request it was for.
		 */
		void traverse(ADC::PeerDial dial, const std::string& host, uint16_t port, uint16_t from);

		std::string peerName() const override { return remote_cid; }

	protected:
		void EventHostLookup(const Samurai::IO::Net::Socket*) override;
		void EventHostFound(const Samurai::IO::Net::Socket*) override;
		void EventConnecting(const Samurai::IO::Net::Socket*) override;
		void EventConnected(const Samurai::IO::Net::Socket*) override;
		void EventTLSConnected(const Samurai::IO::Net::Socket*) override;
		void EventTLSDisconnected(const Samurai::IO::Net::Socket*) override;
		void EventTimeout(const Samurai::IO::Net::Socket*) override;
		void EventDisconnected(const Samurai::IO::Net::Socket*) override;
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override;
		void EventCanWrite(const Samurai::IO::Net::Socket*) override;
		void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char* msg) override;
		void EventTimeout(Samurai::Timer*) override;

		void send(ADC::Command* cmd, bool more = false);

	public:
		void onTransferStopped(QuickDC::Transfer* which) override;
		
	protected:
		/* Held by value, so that a lookup compares the name and not a pointer. */
		std::set<std::string> extensions;
		enum State state;
		ADC::CommandProxy* proxy;
		QuickDC::HubSession* hubsession;
		
		/*
		 * Empty where nobody named one. Neither a SID nor a token nor a CID has
		 * an empty value that means anything, so the two cases do not need
		 * telling apart; what is empty is left out of the command it belongs in.
		 */
		std::string remote_sid;

		/**
		 * The peer's CID, learned from their CINF. Unlike the SID this is
		 * stable across sessions, so it is what a FileSource records.
		 */
		std::string remote_cid;

		std::string token;

		/**
		 * The keyprint the peer's certificate has to match, or unset if there
		 * is nothing trustworthy to check it against.
		 *
		 * An optional rather than a string, because absent is the only thing
		 * that means "check nothing": a keyprint that is there but unreadable
		 * counts as a mismatch and drops the connection, and confusing the two
		 * would leave a peer unauthenticated instead.
		 */
		std::optional<std::string> expected_keyprint;

		/**
		 * Whether we have identified ourselves yet, so that a peer repeating
		 * its CSUP does not have us say who we are twice.
		 */
		bool inf_sent;

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

		ADC::PeerDial dial;
		bool connected;

		std::string traversalHost;
		uint16_t traversalPort;
		uint16_t traversalFrom;
		std::unique_ptr<Samurai::Timer> redial;
		void redialTraversal();

		/** The traversal failed before the handshake was through: say so once. */
		void traversalFailed(const char* why);

		/** The peer wants ADCS, so TLS has to come up before any ADC. */
		bool secure;

		bool upload_approved;
		bool download_approved;

		std::unique_ptr<Samurai::Timer> handshakeDeadline;

		/** Looks every QUICKDC_DOWNLOAD_STALL_SECONDS for data having arrived. */
		std::unique_ptr<Samurai::Timer> stallCheck;
		bool receivedSinceCheck = false;
		void watchForStall();

		/** Tell the peer this connection is for nothing we know of, and hang up. */
		void refuseUnexpected(const char* quoted);

		friend class QuickDC::Connection;
};

}

#endif // HAVE_QUICKDC_ADCCLIENTSESSION_H
