/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"

#include <api/core.h>

#include <map>
#include <vector>
#include <string.h>
#include <string>

#include "config/preferences.h"
#include "network/adc/server/acl.h"
#include "network/adc/server/adchub.h"
#include "network/adc/server/adchubservice.h"
#include "network/adc/server/adchubuser.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/adccommands.h"
#include "network/adc/cid.h"
#include "network/adc/parser.h"
#include <samurai/io/net/socketevent.h>
#include <samurai/io/net/socket.h>
#include <samurai/timestamp.h>

#define FOURCC(a, b, c, d) ((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d))

#define CHECK_SID_OK() \
	ADC::HubUser* user  = lookupUser(ADC::SID::fromString(cmd->getSourceID())); \
	ADC::HubUser* user_sock = lookupUser(cmd->getSocket()); \
	if (cmd->isCommandHub()) user = user_sock; \
	if (user_sock && user_sock != user) { \
		ADC::Command* sta = new ADC::Command(FOURCC('I','S','T','A')); \
		sta->addArgument("240"); \
		sta->addArgument("Protocol violation. SID does not match connection."); \
		send(sta, user_sock); \
		kick(user_sock); \
		\
		if (user_sock && user_sock->state == State_Normal) { \
			ADC::Command* quit = new ADC::Command(FOURCC('I','Q','U','I')); \
			quit->addArgument(ADC::SID::toString(user->getSID())); \
			quit->addArgument("MS", "Protocol violation. SID does not match connection."); \
			send(quit); \
		} \
		\
		return; \
	}

ADC::Hub::Hub() {
	welcome    = "This hub is running " PRODUCT " " VERSION " on " SYSTEM "/" CPU;
	hubname    = 0;
	hubdesc    = 0;
	readConfiguration();

	availSid = 0;

	acl = new ADC::ACL::Table();
	
	/* Start services */
	services = new ADC::ServiceMap(this);
	new ADC::AdcSrvVersion(services);
	new ADC::AdcSrvHelp(services);
	new ADC::AdcSrvUptime(services);
	new ADC::AdcSrvMotd(services);
	new ADC::AdcSrvHistory(services);
	new ADC::AdcSrvMyIP(services);
	
	processing = false;
}

ADC::Hub::~Hub() {
	delete acl;
	delete services;

	while (users.size()) {
		ADC::HubUser* user = users.back();
		userRemove(user);
	}

	while (history.size()) {
		char* remove = history.front();
		history.pop_front();
		free(remove);
	}
	
	free(hubname);
	free(hubdesc);
}

void ADC::Hub::readConfiguration() {
	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Local hub");
	
	free(hubname);
	free(hubdesc);
	
	hubname    = strdup(config->getString("Name", "A QuickDC hub"));
	hubdesc    = strdup(config->getString("Description", "My QuickDC hub"));
	maxUsers   = config->getNumber("Max users", 128);
	maxHistory = config->getNumber("Max history", 50);
	maxSendQ   = config->getNumber("Max send queue bytes", 48 * 1024);
	enabled    = config->getBool("Enabled", false);
	
	if (!enabled) {
		// FIXME: Kill all connections
	}
}

void ADC::Hub::onSUP(ADC::Command* cmd) {
	ADC::HubUser* user = lookupUser(cmd->getSocket());
	if (user && user->state == State_Protocol) {
		if (!enabled) {
			ADC::Command* sta = new ADC::Command(FOURCC('I','S','T','A'));
			sta->addArgument("212");
			sta->addArgument("This hub is disabled");
			send(sta, user);
			kick(user);
			return;
		}

		ADC::Command* c_sup = new ADC::Command(FOURCC('I','S','U','P'));
		ADC::addSupportFlags(*c_sup, false);
		
		ADC::Command* c_sid = new ADC::Command(FOURCC('I','S','I','D'));
		c_sid->addArgument(ADC::SID::toString(user->sid));
		send(c_sup, user, true);
		send(c_sid, user, false);
		user->state = State_Identify;
	}
}

void ADC::Hub::onINF(ADC::Command* cmd) {
	CHECK_SID_OK();
	
	if (user->setINF(cmd, this)) {
		if (user->state == State_Identify) {
			ADC::Command* c_inf = new ADC::Command(FOURCC('I','I','N','F'));
			c_inf->addArgument("NI", hubname);
			c_inf->addArgument("DE", hubdesc);
			c_inf->addArgument("HU", "1");
			c_inf->addArgument("HI", "1");
			c_inf->addArgument("BO", "1");
			c_inf->addArgument("VE", PRODUCT " " VERSION);
			send(c_inf, user, true);
			
			if (users.size() > maxUsers && (user->getCredentials() != Cred_Operator || user->getCredentials() != Cred_Admin)) {
				ADC::Command* c_sta = new ADC::Command(FOURCC('I','S','T','A'));
				c_sta->addArgument("211");
				c_sta->addArgument("This hub is full, please try again later...");
				send(c_sta, user);
				kick(user);

			} else {
				onWelcome(user);
				
				for (size_t n = 0; n < users.size(); n++) {
					ADC::Command* u_inf = users[n]->command_inf;
					if (users[n]->state == State_Normal && u_inf) {
						send(u_inf, user, true);
					}
				}
				
				user->state = State_Normal;
				send(user->command_inf);
				
				// FIXME: If hub is full and an operator/admin was accepted here. Kick some unsuspecting user!
			}
			
		} else {
			if (cmd->countArguments() > 1) {
				send(cmd);
			}
		}
	} else {
		
		if (user->state == State_Identify) {
			kick(user);
		}
	}
}

void ADC::Hub::onRES(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::HubUser* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (target && target->state == State_Normal) {
		send(cmd, target);
	}
}

void ADC::Hub::onMSG(ADC::Command* cmd) {
	CHECK_SID_OK();
	
	ADC::HubUser* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (user->state == State_Normal) {
		if (target) {
			if (target->state == State_Normal)
				send(cmd, target);
		} else {
			if (cmd->getArgument(0) && cmd->getArgument(0)[0] == ADC_HUB_SERVICE_PREFIX)
				services->invoke(cmd);
			else {
				onPublicChat(user, cmd->getArgument(0));
				send(cmd);
			}
		}
	}
}

void ADC::Hub::onSCH(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::HubUser* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (user->state == State_Normal) {
		if (target) {
			if (target->state == State_Normal) {
				send(cmd, target);
			}
		} else {
			send(cmd);
		}
	}
}

void ADC::Hub::onSTA(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::HubUser* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (user->state == State_Normal && target && target->state == State_Normal) {
		send(cmd, target);
	}
}

void ADC::Hub::onCTM(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::HubUser* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (user->state == State_Normal && target && target->state == State_Normal) {
		send(cmd, target);
	}
}

void ADC::Hub::onRCM(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::HubUser* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (user->state == State_Normal && target && target->state == State_Normal) {
		send(cmd, target);
	}
}

void ADC::Hub::onDSC(ADC::Command* cmd) {
	CHECK_SID_OK();
	if (user->credentials == Cred_Operator || user->credentials == Cred_Admin) {
		ADC::HubUser* target = lookupUser(ADC::SID::fromString(cmd->getArgument(0)));
		
		// TODO: Parse different flags from the original kick mesage and pass them along to the kick statement.
		if (target) {
			bool must_send_quit = (target->state == State_Normal);
			ADC::Command* sta = new ADC::Command(FOURCC('I','S','T','A'));
			sta->addArgument("230");
			sta->addArgument("Kicked");
			// sta->addArgument("ID", ADC::SID::toString(user->sid));
			send(sta, target);
			kick(target);
			if (must_send_quit) {
				ADC::Command* quit = new ADC::Command(FOURCC('I','Q','U','I'));
				quit->addArgument(ADC::SID::toString(target->sid));
				quit->addArgument("ID", ADC::SID::toString(user->sid));
				quit->addArgument("MS", "Kicked");
				quit->addArgument("DI");
				send(quit);
			}
		}

	} else {
		QHUB("%s: '%s' tried to disconnect another user. Access denied.", ADC::SID::toString(user->sid), user->nick);
		ADC::Command* c_sta = new ADC::Command(FOURCC('I','S','T','A'));
		c_sta->addArgument("126");
		c_sta->addArgument("Access denied");
		send(c_sta, user);
	}
}

void ADC::Hub::onMHE(ADC::Command*) {
}

void ADC::Hub::onMBO(ADC::Command*) {
}

void ADC::Hub::onPAS(ADC::Command*) {
	// FIXME: Handle password authentication
}

void ADC::Hub::onWelcome(ADC::HubUser* user) {
	QHUB("%s: User logged in: '%s'", ADC::SID::toString(user->sid), user->nick);

	/* Send the welcome message */
	ADC::Command* c_msg = new ADC::Command(FOURCC('I','M','S','G'));
	c_msg->addArgument(welcome);
	send(c_msg, user, true);
	
	/* Send the message of the day (hacked to use the services API) */
	ADC::Command* fake = new ADC::Command(FOURCC('B','M','S','G'), ADC::SID::toString(user->sid), 0);
	fake->setSocket(user->socket);
	fake->addArgument("+motd");
	services->invoke(fake);
}

void ADC::Hub::onQuit(ADC::HubUser* user, const char* msg) {
	if (processing)
	{
		if (user->state == State_Normal) {
			Samurai::postMessage(QuickDC::MsgHubUserDelayedQuit, user, (size_t) (char*) msg, 0);
			user->state = State_WaitDisconnect;
		}
		return;
	}
	
	if (user->locked)
	{
			Samurai::postMessage(QuickDC::MsgHubUserDelayedQuit, user, (size_t) (char*) msg, 0);
			user->state = State_WaitDisconnect;
			return;
	}
	
	QHUB("%s: User disconnected: '%s' (%s)", ADC::SID::toString(user->sid), user->nick, msg);
	userRemove(user);
	if (user->state == State_Normal) {
		ADC::Command* quit = new ADC::Command(FOURCC('I','Q','U','I'));
		quit->addArgument(ADC::SID::toString(user->sid));
		if (msg) quit->addArgument("MS", msg);
		send(quit);
	}
	delete user;
}

void ADC::Hub::onPublicChat(ADC::HubUser* user, const char* line) {
	Samurai::TimeStamp timestamp_obj;
	const char* timestamp = timestamp_obj.getTime("%H:%M");
	char* chat = (char*) malloc(strlen(line) + strlen(timestamp) + strlen(user->nick) + 9);
	sprintf(chat, "[%s] <%s> %s", timestamp, user->nick, line);
	
	if (history.size() > maxHistory-1) {
		char* remove = history.front();
		history.pop_front();
		free(remove);
	}
	history.push_back(chat);
}

void ADC::Hub::send(ADC::Command* cmd, HubUser* user, bool more) {
	cmd->incRef();

	if (user) {
		// Send message directly to one user only
		bool echo = cmd->isCommandEcho();
		user->send(cmd, more);
		if (echo) {
			// If message is an echo message, the sender also want a copy of the message.
			ADC::HubUser* source = lookupUser(ADC::SID::fromString(cmd->getSourceID()));
			if (source) source->send(cmd);
		}
	} else {
		processing = true;
		
		if (!cmd->isCommandFeature()) {
			// Send message to all users
			for (size_t n = 0; n < users.size(); n++) {
				if (users[n]->state == State_Normal) {
					bool send_to_user = true;
					
					// Don't send low priority messages to user if send queue is choked.
					if (users[n]->getSendQueueSize() > maxSendQ && cmd->getPriority() == ADC::Command::Low)
						send_to_user = false;
		
					if (send_to_user)
						users[n]->send(cmd);
				}
			}
		} else {
			// Message is a feature broadcast, only send it to the ones that
			// support the requested feature.
			for (size_t n = 0; n < users.size(); n++) {
				if (users[n]->state == State_Normal) {
					bool send_to_user = true;
					
					// Don't send low priority messages to user if send queue is choked.
					if (users[n]->getSendQueueSize() > maxSendQ && cmd->getPriority() == ADC::Command::Low)
						send_to_user = false;
					
					std::vector<char*>::iterator it;
					for (it = cmd->features_include.begin();
						it != cmd->features_include.end() && send_to_user; it++)
						if (!users[n]->hasFeature((*it))) send_to_user = false;

					for (it = cmd->features_exclude.begin();
						it != cmd->features_exclude.end() && send_to_user; it++)
							if (users[n]->hasFeature((*it))) send_to_user = false;

					if (send_to_user)
						users[n]->send(cmd);
				}
			}
		}
		
		processing = false;
	}
	cmd->decRef();
}


void ADC::Hub::kick(ADC::HubUser* user) {
	user->state = State_WaitDisconnect;
	
	if (user->getSendQueueSize() == 0) {
		user->socket->disconnect();
	}
}


void ADC::Hub::onConnected(Samurai::IO::Net::Socket* socket) {
	ADC::HubUser* user = new ADC::HubUser(socket, this);
	userAdd(user);
}


void ADC::Hub::EventHostLookup(const Samurai::IO::Net::Socket*) { }
void ADC::Hub::EventHostFound(const Samurai::IO::Net::Socket*)  { }
void ADC::Hub::EventConnecting(const Samurai::IO::Net::Socket*) { }
void ADC::Hub::EventTimeout(const Samurai::IO::Net::Socket*)    { }
void ADC::Hub::EventConnected(const Samurai::IO::Net::Socket*)  { }

void ADC::Hub::EventDataAvailable(const Samurai::IO::Net::Socket* socket) {
	ADC::HubUser* user = lookupUser(socket);
	if (user) {
		user->read();
	}
}

void ADC::Hub::EventCanWrite(const Samurai::IO::Net::Socket* socket) {
	ADC::HubUser* user = lookupUser(socket);
	if (user) {
		user->proxy->write();
		if (user->state == State_WaitDisconnect && user->getSendQueueSize() == 0) {
			user->socket->disconnect();
		}
	}
}

void ADC::Hub::EventDisconnected(const Samurai::IO::Net::Socket* socket)  {
	ADC::HubUser* user = lookupUser(socket);
	if (user) {
		onQuit(user);
	}
}

void ADC::Hub::EventError(const Samurai::IO::Net::Socket* socket, Samurai::IO::Net::SocketError, const char* msg)
{
	ADC::HubUser* user = lookupUser(socket);
	if (user) {
		onQuit(user, msg);
	}
}

ADC::HubUser* ADC::Hub::lookupUser(const Samurai::IO::Net::Socket* socket) {
	HubUser* user = 0;
	std::map<const Samurai::IO::Net::Socket*, HubUser*>::iterator socket_it = socket_map.find(socket);
	if (socket_it != socket_map.end()) {
		user = (*socket_it).second;
	}
	return user;
}

ADC::HubUser* ADC::Hub::lookupUser(sid_t sid) {
	HubUser* user = 0;
	std::map<sid_t, HubUser*>::iterator sid_it = sid_map.find(sid);
	if (sid_it != sid_map.end()) {
		user = (*sid_it).second;
	}
	return user;
}

ADC::HubUser* ADC::Hub::lookupUser(const char* nick) {
	for (size_t n = 0; n < users.size(); n++) {
		if (users[n]->state == State_Normal && strcasecmp(users[n]->nick, nick) == 0) {
			return users[n];
		}
	}
	return 0;
}

ADC::HubUser* ADC::Hub::lookupUserCID(const char* cid) {
	for (size_t n = 0; n < users.size(); n++) {
		if (users[n]->state == State_Normal && strcasecmp(users[n]->cid, cid) == 0) {
			return users[n];
		}
	}
	return 0;
}

void ADC::Hub::userAdd(HubUser* user) {
	if (processing) {
		Samurai::postMessage(QuickDC::MsgHubUserAppendUser, user, 0, 0);
		return;
	}

	socket_map.insert(std::pair<Samurai::IO::Net::Socket*, HubUser*>(user->socket, user));
	sid_map.insert(std::pair<sid_t, HubUser*>(user->sid, user));
	users.push_back(user);
}

void ADC::Hub::userRemove(HubUser* user) {
	if (processing) {
		Samurai::postMessage(QuickDC::MsgHubUserRemoveUser, user, 0, 0);
		return;
	}

	if (user) {
		std::map<const Samurai::IO::Net::Socket*, HubUser*>::iterator socket_it = socket_map.find(user->socket);
		if (socket_it != socket_map.end()) {
			socket_map.erase(socket_it);
		}
		std::map<sid_t, HubUser*>::iterator sid_it = sid_map.find(user->sid);
		if (sid_it != sid_map.end()) {
			sid_map.erase(sid_it);
		}
		std::vector<HubUser*>::iterator user_it = users.begin();
		for (; *user_it; user_it++) {
			if (*user_it == user) {
				users.erase(user_it);
				break;
			}
		}
	}
}

sid_t ADC::Hub::generateNextSid()
{
	while (lookupUser(availSid++ % 1048576) && availSid != 0) { }
	return availSid;
}

void ADC::Hub::EventTimeout(Samurai::Timer*) { }

bool ADC::Hub::EventMessage(const Samurai::Message* msg) {
	if (!processing) {
		if (msg->getID() == QuickDC::MsgHubUserRemoveUser)
		{
			ADC::HubUser* user = (ADC::HubUser*) msg->getData();
			userRemove(user);
			return true;
		}
		
		if (msg->getID() == QuickDC::MsgHubUserAppendUser)
		{
 			ADC::HubUser* user = (ADC::HubUser*) msg->getData();
			userAdd(user);
			return true;
		}
		
		if (msg->getID() == QuickDC::MsgHubUserDelayedQuit)
		{
			ADC::HubUser* user = (ADC::HubUser*) msg->getData();
			const char* msg = "Delayed quit"; // FIXME: Retrieve this from msg.
			onQuit(user, msg);
			return true;
		}
	
	} else {
		QHUB("Parsing messages while processing...");
	}
	return false;
}

void ADC::Hub::ensureUserLimit() {
	if (users.size() <= maxUsers) {
		return;
	}
	
}
