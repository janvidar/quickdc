/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCHUB_H
#define HAVE_QUICKDC_ADCHUB_H

#include "quickdc.h"
#include <string.h>
#include <memory>
#include <map>
#include <vector>
#include <deque>
#include <string>
#include "network/adc/adccommon.h"
#include "network/clientsession.h"
#include <samurai/io/net/socketevent.h>
#include <samurai/timer.h>

namespace Samurai {
	namespace IO {
		namespace Net {
			class Socket;
			class InetAddress;
			class DatagramSocket;
			class DatagramPacket;
		}
	}
}

namespace ADC {
	class Command;
	class CommandProxy;
	class Hub;
	class ServiceMap;
	class HubUser;

	/** How often the hub looks over its connections for ones that have gone quiet. */
	inline constexpr time_t HUB_SWEEP_INTERVAL = 10;

	/** A SID is 20 bits, which is what SID::toString() renders and nothing more. */
	inline constexpr sid_t SID_MASK = 0xFFFFF;

	namespace ACL {
		class Table;
	}

	class Hub :
		public Samurai::IO::Net::SocketEventHandler,
		public Samurai::TimerListener,
		public Samurai::MessageListener
	{
		public:
			Hub();
			virtual ~Hub();

			Hub(const Hub&) = delete;
			Hub& operator=(const Hub&) = delete;
	
			void onINF(ADC::Command* cmd);
			void onRES(ADC::Command* cmd);
			void onSUP(ADC::Command* cmd);
			void onMSG(ADC::Command* cmd);
			void onSCH(ADC::Command* cmd);
			void onCTM(ADC::Command* cmd);
			void onRCM(ADC::Command* cmd);
			void onDSC(ADC::Command* cmd);
			void onPAS(ADC::Command* cmd);
			void onSTA(ADC::Command* cmd);
			
			void onECO(ADC::Command* cmd);
			void onTUN(ADC::Command* cmd);
			
			// BBS extension
			void onMHE(ADC::Command* cmd);
			void onMBO(ADC::Command* cmd);
	
			/**
			* This method is used for sending/distributing messages to users.
			* A command can be sent to one user, or to multiple users.
			*
			* @param cmd The command to be sent.
			* @param user If the command is to be sent to one user only, a user must be specified.
			* @param more If 'true', no attempt is made to flush the send queue. 'false' will flush it immediately.
			*/
			void send(ADC::Command* cmd, ADC::HubUser* user = 0, bool more = false);
			
			/**
			* This method is used for disconnecting a user from the hub.
			*/
			void kick(ADC::HubUser* user);
			
			/**
			* This method is called when a user has connected to the
			* hub but not yet logged in properly.
			*/
			void onConnected(std::shared_ptr<Samurai::IO::Net::Socket> socket);

			/**
			* Decide whether a newly probed connection may become a user at all.
			*
			* The hub does not authenticate, so this is the only thing standing
			* between an anonymous peer and a HubUser with a send queue, a SID and
			* a command proxy of its own. Nothing here has read a byte from the
			* connection yet.
			*
			* @return false if the connection is to be dropped.
			*/
			bool admitConnection(const Samurai::IO::Net::Socket* socket);

			/** Users that have completed the login exchange. */
			size_t countLoggedIn() const;

			/** Users that have connected but not yet completed the login exchange. */
			size_t countHandshaking() const;

			/** Users connected from the given address. */
			size_t countFromAddress(const Samurai::IO::Net::InetAddress& address) const;

			/**
			* This method is called when a user is logged in, and will
			* notify all the other users about this user.
			*/
			void onWelcome(HubUser*);
			
			/**
			* This method is called when a user sends a public message.
			* It can be used to intercept the message, etc.
			*/
			void onPublicChat(HubUser* user, const char* line);
			
			/**
			* This method is called when a user connection is dropped,
			* with an optional message attached to it.
			* This will ensure all the other users are notified about
			* it.
			*/
			void onQuit(HubUser* user, const char* msg = 0);
			
		protected:
			void EventHostLookup(const Samurai::IO::Net::Socket*) override;
			void EventHostFound(const Samurai::IO::Net::Socket*) override;
			void EventConnecting(const Samurai::IO::Net::Socket*) override;
			void EventConnected(const Samurai::IO::Net::Socket*) override;
			void EventTimeout(const Samurai::IO::Net::Socket*) override;
			void EventTimeout(Samurai::Timer*) override;
			void EventDisconnected(const Samurai::IO::Net::Socket*) override;
			void EventDataAvailable(const Samurai::IO::Net::Socket*) override;
			void EventCanWrite(const Samurai::IO::Net::Socket*) override;
			void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char* msg) override;
			bool EventMessage(const Samurai::Message*) override;
	
		public:
			/**
			* Lookup a user based on a socket.
			* @return 0 if no user is found
			*/
			HubUser* lookupUser(const Samurai::IO::Net::Socket*);
			
			/**
			* Lookup a user based on a session ID.
			* @return 0 if no user is found
			*/
			HubUser* lookupUser(sid_t sid);
			
			/**
			* Lookup a user based on a nick name.
			* @return 0 if no user is found
			*/
			HubUser* lookupUser(const char* nick);
			
			/**
			* Lookup a user based on a client ID (CID).
			* @return 0 if no user is found
			*/
			HubUser* lookupUserCID(const char* cid);
	
		
		protected:
			/**
			* Add a user to the user database.
			* If the database is locked (processed), a message will be posted, and the operation will
			* be performed later, when the message queue is processed.
			*/
			void userAdd(HubUser*);
			
			/**
			* Remove a user from the user database.
			* If the database is locked (processed), a message will be posted and the operation will
			* be performed later, when the message queue is processed.
			*/
			void userRemove(HubUser*);

			/**
			* Register a user for deferred work and return the ticket naming it.
			*
			* Deferred work used to carry the HubUser pointer itself, which meant a
			* user deleted before its message was processed left the handler holding
			* freed memory - and nothing withdrew the message. A ticket is looked up
			* instead, and forgetPending() invalidates every ticket a user holds when
			* it goes away, so a stale message resolves to nothing.
			*/
			uint64_t deferFor(HubUser* user);

			/** Resolve and consume a ticket. 0 if the user is already gone. */
			HubUser* takePending(uint64_t ticket);

			/** Drop every outstanding ticket naming this user. */
			void forgetPending(const HubUser* user);

			/**
			* Generate a unique session ID for a new user. This is used for new connections only,
			* and will guarantee no duplicates.
			*
			* NOTE: Due to limitations in the ADC specification, only 1 million users can
			* ever exist on one logical hub.
			*/
			sid_t generateNextSid();
			
			/**
			* (Re-)read the configuration file, and apply the hub specific settings.
			*/
			void readConfiguration();
			
			/**
			* Tell a user it is being dropped, and drop it.
			*/
			void expire(HubUser* user, const char* code, const char* reason);

		public:
			char* hubname;
			char* hubdesc;
			const char* welcome;
			uint32_t maxUsers;
			uint32_t maxHistory;

			/** Beyond this a user is skipped for anything but a High priority message. */
			uint32_t maxSendQ;

			/** And beyond this it is dropped: it is not reading at all. */
			uint32_t hardSendQ;

			/** Connections allowed from one address at a time. */
			uint32_t maxPerAddress;

			/** Connections allowed to sit in the login exchange at once. */
			uint32_t maxHandshake;

			/** Seconds a connection may take over the login exchange, 0 to allow forever. */
			uint32_t handshakeTimeout;

			/** Seconds a logged in user may be silent for, 0 to allow forever. */
			uint32_t idleTimeout;

			bool enabled;

			sid_t availSid;
			
			/* Quick lookup maps */
			std::map<const Samurai::IO::Net::Socket*, HubUser*> socket_map;
			std::map<sid_t, HubUser*>         sid_map;
			std::vector<HubUser*>                users;
			ServiceMap* services;
			std::deque<char*> history;
	
			bool processing;

			/* Outstanding deferred work, keyed by ticket. See deferFor(). */
			std::map<uint64_t, HubUser*> pending;
			uint64_t nextPendingTicket;

			/* ACL */
			ACL::Table* acl;

			/*
			 * Sweeps the connections. Owned here rather than one per user: a hub
			 * with a timer each would put every connection's deadline in the
			 * manager, and the sweep is the only thing that ever reads them.
			 */
			Samurai::Timer* timer;
			
	
		friend class ADC::HubUser;
	};

}

#endif // HAVE_QUICKDC_ADCHUB_H
