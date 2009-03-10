/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCHUB_H
#define HAVE_QUICKDC_ADCHUB_H

#include "quickdc.h"
#include <string.h>
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
			void onConnected(Samurai::IO::Net::Socket* socket);
			
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
			void EventHostLookup(const Samurai::IO::Net::Socket*);
			void EventHostFound(const Samurai::IO::Net::Socket*);
			void EventConnecting(const Samurai::IO::Net::Socket*);
			void EventConnected(const Samurai::IO::Net::Socket*);
			void EventTimeout(const Samurai::IO::Net::Socket*);
			void EventTimeout(Samurai::Timer*);
			void EventDisconnected(const Samurai::IO::Net::Socket*);
			void EventDataAvailable(const Samurai::IO::Net::Socket*);
			void EventCanWrite(const Samurai::IO::Net::Socket*);
			void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char* msg);
			bool EventMessage(const Samurai::Message*);
	
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
			* The purpose of this method is to ensure that the number
			* of users will not be kept greater than the set maxUsers variable.
			* This can be used to kick out users when operators entered a full hub,
			* or to kick out users when configuration changed to a lower maxUsers
			* variable. One can also set maxUsers to 0, and call this function to kick everybody
			* out, but only users not in State_Normal mode is affected by this.
			*
			* This function will try to kick the users with the longest idle time, and/or
			* longest message send queue (typically slow connections).
			*/
			void ensureUserLimit();
			
		public:
			char* hubname;
			char* hubdesc;
			const char* welcome;
			uint32_t maxUsers;
			uint32_t maxHistory;
			uint32_t maxSendQ;
			bool enabled;
			
			sid_t availSid;
			
			/* Quick lookup maps */
			std::map<const Samurai::IO::Net::Socket*, HubUser*> socket_map;
			std::map<sid_t, HubUser*>         sid_map;
			std::vector<HubUser*>                users;
			ServiceMap* services;
			std::deque<char*> history;
	
			bool processing;
			
			/* ACL */
			ACL::Table* acl;
			
	
		friend class ADC::HubUser;
	};

}

#endif // HAVE_QUICKDC_ADCHUB_H
