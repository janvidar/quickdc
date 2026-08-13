/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCHUBSESSION_H
#define HAVE_QUICKDC_ADCHUBSESSION_H

#include <string.h>
#include "network/adc/adccommon.h"
#include "network/connection.h"
#include "network/hubsession.h"
#include "network/user.h"
#include "network/usermanager.h"
#include "share/searchrequest.h"
#include "config/preferences.h"
#include "api/bulletins.h"
#include "api/hub.h"

#include <samurai/timer.h>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>


namespace QuickDC {
struct PeerEndpoint;
class Hub;
class User;
class SearchRequest;
class HubSession;
class UserEventHandler;
class HubListener;
namespace Share {
class SearchResult;
class SearchRequest;
}
}


/*
 * The shortest salt this client will answer a password request with, in bytes
 * once decoded.
 *
 * The hub picks the salt and we hash the password with it, so a salt with nothing
 * in it makes the answer a function of the password alone - the same twenty-six
 * characters every time, for ever, replayable by anyone who ever saw one. A single
 * base32 character decodes to no bytes at all, which is exactly that case. 128
 * bits is the floor at which the answer is worth something.
 */
#define QUICKDC_ADC_SALT_MIN_BYTES 16

/*
 * Where the salt a hub last asked with is filed, against that hub.
 *
 * One value rather than a set: the exchange happens once per connection, so a hub
 * that reuses a salt does it on the next login rather than twice on this one.
 */
#define QUICKDC_ADC_SALT_SETTING "Last password salt"

/**
 * How long an ADC connection may sit silent before a newline is sent over it.
 *
 * Two minutes, which is inside the idle timeout of every connection tracking
 * firewall this is meant to survive - they commonly forget a TCP connection
 * after five. Overridden by [Network] Keep alive interval; zero turns it off.
 */
#define QUICKDC_ADC_KEEPALIVE 120

namespace ADC {
class CommandProxy;
class Command;

/**
 * An INF field and what this client has to say for it.
 *
 * An empty value means it has nothing to say - no e-mail was configured, there
 * is no certificate to take a keyprint of, no port because nobody can connect
 * here. Every field this client manages appears with a value either way, which
 * is what lets one rule cover both leaving an unset field out of the first INF
 * and telling the hub to drop a field that has since become unset.
 */
using InfField = std::pair<std::string, std::string>;

/**
 * A class describing a current hub session.
 * In other words, a class holding all info related to a hub
 * connection.
 */
class HubSession :
		public QuickDC::HubSession,
		public QuickDC::UserEventHandler,
		public Samurai::TimerListener,
		public QuickDC::Connection,
		public QuickDC::Share::SearchReplyHandler
{
	public:
		HubSession(QuickDC::Hub* ui, QuickDC::HubListener* listener, Samurai::IO::Net::URL* url, bool secure);
		virtual ~HubSession();

		/* External API */
		void sendPrivateChatMessage(const QuickDC::User*, const char*,
		                            bool richText = false) override;
		void sendChatMessage(const char*, bool richText = false) override;
		void sendAdminUserKick(const QuickDC::User*) override;
		void sendAdminUserRedirect(const QuickDC::User*, const char*, const char*) override;
		void sendClientInfo() override;
		void sendSearchRequest(QuickDC::Share::SearchRequest* request) override;
		void sendConnectionRequest(const QuickDC::User*, const char* token) override;

		/*
		 * The bulletin board commands a client sends: BBL to read a board and BBP
		 * to write to one.
		 *
		 * Each refuses unless the hub has announced BBS0 and the session is in the
		 * NORMAL state, and each checks its own arguments before writing anything.
		 * The hub's announcement is what makes the commands legal on this
		 * connection at all - a client that sends them on a hub which has said
		 * nothing is speaking a protocol nobody agreed to - and the state is
		 * because a board is not a thing that exists during a login.
		 *
		 * The arguments are checked here rather than left for the hub to refuse
		 * because a malformed board name or hash reaching this point is a fault in
		 * this client, and the answer to one is not a status message from a stranger
		 * describing it.
		 */
		bool subscribeBoard(const char* board, uint64_t timestamp) override;
		bool unsubscribeBoard(const char* board) override;
		bool requestEntry(const char* board, const char* tth) override;
		bool submitPost(const char* tth, uint64_t size, const char* board,
		                const char* subject, const char* parent) override;
		bool withdrawPost(const char* tth, const char* board) override;

		/**
		 * Forget what this hub was told, so the next INF is a whole one again.
		 *
		 * Called when the hub assigns a SID, which is where a session begins and
		 * therefore where the hub stops holding anything an earlier one said.
		 */
		void forgetAnnouncedInfo();

		/**
		 * The address to publish in an INF, whether or not anyone can connect to
		 * it. Published even when passive, because NAT traversal cannot work
		 * without the hub forwarding it.
		 */
		std::string announcedAddress(const QuickDC::PeerEndpoint& endpoint) const;
		bool knowUser(const char* username);
		const QuickDC::User* getUser(const char* username) override;
		const QuickDC::User* getLocalUser() override;
		QuickDC::UserManager* getUserManager() override;
		
		// Events triggered by the server (hub)
		void onSID(char* sid);
		void addSupport(const char* txt);

		/**
		 * Forget a feature the hub has withdrawn, named with 'RM' in a later
		 * SUP. What the hub currently claims is the only thing consulted; see
		 * the definition.
		 */
		void removeSupport(const char* txt);

		/** Called once the hub's entire SUP list has been read. */
		void onSupports();

		void onHubName(char* txt);
		void onHubDesc(char* txt);
		void onHubVersion(char* txt);

		void onUserInfo(const char* id, QuickDC::User* data, uint64_t oldShare, bool changed);
		void onStatus(ADC::Command* cmd);

		/**
		 * BBD: the hub describing one of its boards, or saying that one is gone.
		 *
		 * A descriptor says what there is to subscribe to and subscribes nobody to
		 * anything. Its absence is the whole of the mechanism for hiding a board, so
		 * a board never described is one this session may not see rather than one
		 * that does not exist.
		 */
		void onBoardDescription(const QuickDC::Board& board);

		/**
		 * BBL: one index entry, arriving because this session subscribed to the
		 * board, asked for the single post, or wrote it.
		 *
		 * Reported as it arrives and in the order it arrived. Nothing here decides
		 * what a board looks like: entries are keyed on the tth by whoever keeps
		 * them, since resuming a subscription deliberately replays the second it
		 * resumed from, and a withdrawal is a second entry for a tth already held.
		 */
		void onBoardEntry(const QuickDC::BoardEntry& entry);

		void onPublicChat(const char* from, const char* message, bool action,
		                  bool richText, uint64_t timestamp);
		void onPrivateChat(const char* to, const char* from, const char* message,
		                   const char* contextID, bool action, bool richText,
		                   uint64_t timestamp);
		void onRequestPasword(char* token);

		/**
		 * Whether a salt is one worth answering: long enough to make the answer
		 * depend on it, and not one this session has already answered.
		 *
		 * Reports through the listener when it is not, because the visible effect
		 * otherwise is only that the login does not complete.
		 */
		bool saltIsUsable(const std::string& salt);
		void sendPassword(const char* password) override;
		void sendPasswordResponse();

		void onActiveConnect(const char* from, const char* protocol, uint16_t port, const char* token);
		void onPassiveConnect(const char* from, const char* protocol, const char* token);

		/**
		 * NATT: the peer named the port it will dial from, so dial it back from
		 * ours and tell it which that is. Both sides dialling at once is what
		 * carries the connection through two NATs.
		 */
		void onNatConnect(const char* from, const char* protocol, uint16_t port,
			const char* token);

		/** The answer to our own DNAT: dial, and say nothing further. */
		void onNatReply(const char* from, const char* protocol, uint16_t port,
			const char* token);
		void onSearch(QuickDC::Share::SearchRequest* request);
		void onSearchResult(QuickDC::Share::SearchResult* result) override;

		/** A peer told us which parts of a queued file it holds (PFSR). */
		void onPartialStatus(const char* cid, const char* tth, const char* indexes);

		/** Offer what we have of a queued file the searcher wanted. */
		void sendPartialStatus(QuickDC::Share::SearchRequest* request);
		
		void sendPeerError(const char* sid, const char* code, const char* msg, const char* token, const char* protocol) override;

	protected:
		/**
		 * Whether this session may say anything about a board at all.
		 *
		 * Two conditions, and both of them are the hub's. It must have announced
		 * BBS0 - the announcement is authoritative for the connection, is not
		 * cached across one, and may be withdrawn while one lasts - and the session
		 * must have reached the NORMAL state, which is where the hub has a user to
		 * attribute a post to and boards to attribute it on. A client that asks
		 * anyway is refused once per command for as long as it keeps asking.
		 */
		bool mayUseBulletins() const;

		/**
		 * Refuse a peer connection that would not be encrypted, telling both the
		 * peer and whoever is watching this session why.
		 */
		void refuseUnencrypted(const char* from, const char* token,
		                       const char* proto, const char* why);

		/**
		 * Whether a peer connection arranged through this hub has to be encrypted.
		 *
		 * A hub reached over ADCS answers yes and is not asked anything further:
		 * the peer connections it brokers carry the same files the session does,
		 * and a client that took the trouble to reach the hub encrypted has said
		 * what it wants. Plaintext there is a refusal rather than a fallback,
		 * which is why this is not a preference in that case.
		 *
		 * A plain ADC hub leaves it to the setting, which is off; there ADC/1.0 is
		 * what a peer without ADCS has, and it is accepted.
		 */
		bool mustEncryptPeers() const;

		/**
		 * The protocol to name in a CTM or RCM for this user, read off the SU
		 * field of their INF.
		 *
		 * @param listening whether we are the end that will be dialled, in which
		 *        case naming encryption also requires a certificate to answer
		 *        with - see canAcceptTls(). Dialling out needs none.
		 * @return the protocol, or 0 when nothing can be arranged that this hub
		 *         allows, which is the case the caller has to abandon.
		 */
		const char* peerConnectProtocol(const QuickDC::User* user, bool listening) const;

		/**
		 * The local port of this hub connection, which is what a NATT exchange
		 * names: the mapping our NAT made for it is the one a peer's connection
		 * can arrive on. Zero when there is no connection to read it from, or when
		 * it would mean nothing - a proxied socket's local port reaches the proxy.
		 */
		/** Whether NAT traversal is possible on this connection at all. */
		bool canTraverse() const;

		uint16_t privateEndpointPort() const;

		/**
		 * Dial a peer from this hub connection's own local port.
		 *
		 * @return false if the port could not be had, in which case nothing was
		 *         started and the caller has nothing to wait for.
		 */
		bool dialFromHubPort(const QuickDC::User* user, const char* proto,
			uint16_t port, const char* token);

		/**
		 * Everything an INF would say about this client, in the order it goes
		 * out in.
		 *
		 * Not const: working out what to publish is also where the reachability
		 * record is told what is being claimed, and that has to happen on every
		 * pass whether or not anything is left to send afterwards.
		 */
		void collectClientInfo(std::vector<InfField>& fields);

		void send(ADC::Command* cmd, bool more = false);
		void resetIdleTimer();
		
	public:
		virtual void EventHostLookup(const Samurai::IO::Net::Socket*) override;
		virtual void EventHostFound(const Samurai::IO::Net::Socket*) override;
		virtual void EventConnecting(const Samurai::IO::Net::Socket*) override;
		virtual void EventConnected(const Samurai::IO::Net::Socket*) override;
		virtual void EventTimeout(const Samurai::IO::Net::Socket*) override;
		virtual void EventDisconnected(const Samurai::IO::Net::Socket*) override;
		virtual void EventDataAvailable(const Samurai::IO::Net::Socket*) override;
		virtual void EventCanWrite(const Samurai::IO::Net::Socket*) override;
		virtual void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char*) override;
		virtual void EventTLSConnected(const Samurai::IO::Net::Socket*) override;
		virtual void EventTLSDisconnected(const Samurai::IO::Net::Socket*) override;
		
		virtual void EventSearchReply(QuickDC::Share::SearchReply*) override;
		virtual void connect() override;
		virtual void disconnect() override;
		virtual bool isLoggedIn() const override;
		virtual bool isSecure() const override;
		virtual bool supportsRichText() const override;
		bool supportsBulletins() const override { return supports(ADC_EXT_BULLETIN); }

		/**
		 * Check if the server supports a given feature.
		 */
		bool supports(const char* feature) const;

	protected:
		/**
		 * KEYP: compare the hub's certificate against the keyprint we expect,
		 * before a single byte of the session is sent.
		 *
		 * @return false if the connection was dropped, and the caller must
		 *         stop what it was doing.
		 */
		bool verifyKeyprint();

		/** Report a keyprint failure and drop the connection for good. */
		void reportKeyprintFailure(const char* msg);

		/** The session group a remembered keyprint is filed under. */
		const char* keyprintGroup() const;

		/**
		 * Our own SID as the command constructors take it, which is null until
		 * the hub has given us one. A command that would name us as its source
		 * is not written at all while that is the case.
		 */
		const char* localSid() const { return sid.empty() ? 0 : sid.c_str(); }

	private:
		void EventUserJoin(const char* nick) override;
		void EventUserQuit(const QuickDC::User* nick, const char* msg) override;
		void EventUserInfo(const QuickDC::User* data) override;
		void EventTimeout(Samurai::Timer* timer) override;
		
	protected:
		std::set<std::string> extensions;
		QuickDC::Preferences* session;
		std::string address;

		ADC::CommandProxy* proxy;
		QuickDC::HubListener* listener;

		/**
		 * Sends a newline when nothing else has been sent or received for a
		 * while; see resetIdleTimer().
		 */
		Samurai::Timer* infoTimer;

		/**
		 * How long the connection may be silent before that newline goes, in
		 * seconds. [Network] Keep alive interval, and 120 when it says nothing.
		 */
		time_t keepAlive;

		enum UserState userState;
		
	protected:
		/*
		 * All four come from the session or configuration file, so they are
		 * always a value and never an absence - an empty description or e-mail
		 * is a field the user left blank, and goes out of the INF as nothing
		 * rather than as a field with nothing in it.
		 */
		std::string nickname;
		std::string description;
		std::string email;
		std::string password;

		/* The token the hub sent with its request. The answer is derived from
		   it, so it has to outlive the request while a password is found. */
		std::string passwordToken;

		/*
		 * Our session ID, as the hub assigned it, and empty before it has. A
		 * SID is four base32 characters, so an empty one is no SID rather than
		 * a short one; the same goes for the hub's own name and version.
		 */
		std::string sid;

		std::string hubname;
		std::string hubdesc;
		std::string hubversion;
		bool secure;

		/**
		 * Where this hub says it sees us, and empty until it has said.
		 *
		 * Per hub rather than per client: the address a hub observes is the one it
		 * will hand to peers on that hub, and two hubs reached over different
		 * routes need not agree. Set from a status naming an address, which is a
		 * hub refusing what we claimed and saying what it saw instead.
		 */
		std::string observedAddress;

		/**
		 * Every INF field this hub has been told, and the value it was told, so
		 * a later INF can carry the difference rather than the whole of it.
		 *
		 * A hub merges an INF into what it already holds and relays only what
		 * arrived, so repeating the unchanged fields buys nothing and costs a
		 * line the length of the whole record - and 'PD' may not be repeated at
		 * all. Empty until this hub has been told anything, which is also what a
		 * reconnection puts it back to.
		 *
		 * In order, rather than looked up by name: the order fields go out in is
		 * the order they are collected in, and keeping it makes a capture read
		 * the same way twice.
		 */
		std::vector<InfField> announcedInfo;

		/**
		 * The keyprint from the 'kp' parameter of the hub address, if any.
		 *
		 * Unset is the only thing that means "check nothing", which is why this
		 * is an optional rather than a string that happens to be empty: a
		 * keyprint that is there but unusable stops the connection instead, and
		 * the difference between the two is the whole of the authentication.
		 */
		std::optional<std::string> expectedKeyprint;

		/**
		 * Set once the hub has presented a certificate matching a keyprint we
		 * already had - either one that came with the hub address, or the one
		 * this hub used the last time we were here.
		 *
		 * This is what decides whether the KP fields the hub relays for other
		 * users can be believed: a hub connection nobody has vouched for could
		 * have had those fields rewritten en route.
		 */
		bool keyprintVerified;

	public:
		QuickDC::UserManager* users;
		
	friend class QuickDC::HubManager;
	friend class QuickDC::Hub;
};

}

#endif // HAVE_QUICKDC_ADCHUBSESSION_H
