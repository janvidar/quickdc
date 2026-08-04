/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"

#include <api/core.h>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>
#include <string.h>
#include <string>
#include <string_view>

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
#include <samurai/io/net/inetaddress.h>
#include <samurai/timer.h>
#include <samurai/timestamp.h>

/*
 * Where a socket's peer is, for the log.
 *
 * getAddress() answers null for a socket that has no peer - one that never
 * connected, or whose peer has already gone - and a connection is refused, or
 * dropped, precisely when that may have happened, so the two lines below used to
 * dereference null on the case they were reporting.
 *
 * A placeholder rather than nothing, because this one is only ever printed;
 * where an address is compared, an unknown one has to stay unknown.
 *
 * Only the log reads it, and the log is compiled out unless this is a tracing
 * build, which is what the attribute is for.
 */
[[maybe_unused]] static std::string peerAddress(const Samurai::IO::Net::Socket* socket)
{
	if (!socket) return std::string("(unknown)");
	const Samurai::IO::Net::InetAddress* addr = socket->getAddress();
	return addr ? addr->toString() : std::string("(unknown)");
}

#define CHECK_SID_OK() \
	ADC::HubUser* user  = lookupUser(ADC::SID::fromString(cmd->getSourceID())); \
	ADC::HubUser* user_sock = lookupUser(cmd->getSocket()); \
	if (cmd->isCommandHub()) user = user_sock; \
	if (user_sock && user_sock != user) { \
		/* \
		 * The connection is retired by expire(), which names it by its own SID \
		 * and decides whether it owes anyone a QUIT before kick() moves the \
		 * state: 'user' is null whenever the claimed SID named nobody at all, \
		 * and announcing the claim instead would have every other user retire \
		 * whichever user it did name. \
		 */ \
		expire(user_sock, "240", "Protocol violation. SID does not match connection."); \
		return; \
	} \
	if (!user) return; \
	if (!user->allowCommand()) { \
		/* One message and out, so answering a flood is not itself amplification. \
		   Nothing more is read from a kicked connection. */ \
		expire(user, "212", "Flooding the hub"); \
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
	nextPendingTicket = 1;

	timer = new Samurai::Timer(this, ADC::HUB_SWEEP_INTERVAL, false);
}

uint64_t ADC::Hub::deferFor(ADC::HubUser* user)
{
	const uint64_t ticket = nextPendingTicket++;
	pending.insert(std::pair<uint64_t, HubUser*>(ticket, user));
	return ticket;
}

ADC::HubUser* ADC::Hub::takePending(uint64_t ticket)
{
	std::map<uint64_t, HubUser*>::iterator it = pending.find(ticket);
	if (it == pending.end()) return 0;

	HubUser* user = (*it).second;
	pending.erase(it);
	return user;
}

void ADC::Hub::forgetPending(const ADC::HubUser* user)
{
	std::map<uint64_t, HubUser*>::iterator it = pending.begin();
	while (it != pending.end())
	{
		if ((*it).second == user) it = pending.erase(it);
		else it++;
	}
}

ADC::Hub::~Hub() {
	delete timer; timer = 0;
	delete acl;
	delete services;

	/*
	 * Deleted, not merely unmapped. A user holds the last reference to its socket
	 * and the socket's event handler is this hub, so a user left behind here keeps
	 * a registered socket pointing at a hub that no longer exists - and the next
	 * readiness event on it is dispatched into freed memory.
	 */
	while (users.size()) {
		ADC::HubUser* user = users.back();
		userRemove(user);
		forgetPending(user);
		delete user;
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

	/*
	 * Every one of these is clamped, because each is an int from a file the hub
	 * does not write and is used as an unsigned bound. A configured 0 for the
	 * history turned 'history.size() > maxHistory-1' into a comparison against
	 * 0xFFFFFFFF, so the ring never rotated and the chat log grew for as long as
	 * the hub ran.
	 */
	maxUsers   = static_cast<uint32_t>(std::clamp(config->getNumber("Max users", 128), 1, 65536));
	maxHistory = static_cast<uint32_t>(std::clamp(config->getNumber("Max history", 50), 0, 65536));
	maxSendQ   = static_cast<uint32_t>(std::clamp(config->getNumber("Max send queue bytes", 48 * 1024),
	                                              4 * 1024, 16 * 1024 * 1024));
	maxPerAddress = static_cast<uint32_t>(std::clamp(config->getNumber("Max connections per address", 8), 1, 65536));
	maxHandshake  = static_cast<uint32_t>(std::clamp(config->getNumber("Max connections in handshake", 32), 1, 65536));
	handshakeTimeout = static_cast<uint32_t>(std::clamp(config->getNumber("Handshake timeout", 30), 0, 86400));
	idleTimeout      = static_cast<uint32_t>(std::clamp(config->getNumber("Idle timeout", 900), 0, 86400));
	enabled    = config->getBool("Enabled", false);

	/* Choking a user is a hint that it is slow; this is the point at which it is
	   not reading at all, and holding anything more for it is pointless. */
	hardSendQ  = maxSendQ * 4;

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
			
			/*
			 * A slot is held by a user that has logged in; this one has not yet,
			 * so it is not among the ones being counted. The exemption is an
			 * 'and': written as an 'or' it held for every user, operators
			 * included, so a full hub turned them away too.
			 */
			if (countLoggedIn() >= maxUsers
				&& user->getCredentials() != Cred_Operator
				&& user->getCredentials() != Cred_Admin) {
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
			/*
			 * Held in a local and tested once. 'BMSG <sid>' parses cleanly with
			 * the only argument consumed as the source ID, which leaves no
			 * message at all - and the else branch went on to hand that to
			 * strlen().
			 */
			const char* line = cmd->getArgument(0);
			if (!line) return;

			if (line[0] == ADC_HUB_SERVICE_PREFIX)
				services->invoke(cmd);
			else {
				onPublicChat(user, line);
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
	/* Owned here: a service answers with a command of its own and keeps no
	   reference to the one it was asked with. */
	std::unique_ptr<ADC::Command> fake(
		new ADC::Command(FOURCC('B','M','S','G'), ADC::SID::toString(user->sid), 0));
	fake->setSocket(user->socket.get());
	fake->addArgument("+motd");
	services->invoke(fake.get());
}

void ADC::Hub::onQuit(ADC::HubUser* user, const char* msg) {
	/*
	 * Decide once, on the first pass, whether anyone was ever told about this
	 * user. The state is about to become State_WaitDisconnect, and a deferred
	 * pass reading it afterwards would conclude the user had never been visible
	 * and skip the IQUI, leaving every remaining user a ghost entry for good.
	 */
	if (!user->quitting)
	{
		user->quitting = true;
		user->announce_quit = (user->state == State_Normal);
	}

	if (processing || user->locked)
	{
		/*
		 * Every quit is deferred here, not only a State_Normal one: dropping the
		 * others outright left the user in socket_map, sid_map and users with
		 * nothing queued to ever take it out again.
		 */
		user->state = State_WaitDisconnect;
		Samurai::postMessage(QuickDC::MsgHubUserDelayedQuit, 0, static_cast<size_t>(deferFor(user)), 0);
		return;
	}

	QHUB("%s: User disconnected: '%s' (%s)", ADC::SID::toString(user->sid),
		user->nick ? user->nick : "(unidentified)", msg ? msg : "no reason given");

	userRemove(user);
	if (user->announce_quit) {
		ADC::Command* quit = new ADC::Command(FOURCC('I','Q','U','I'));
		quit->addArgument(ADC::SID::toString(user->sid));
		if (msg) quit->addArgument("MS", msg);
		send(quit);
	}

	/* Any ticket still naming this user must stop resolving to it. */
	forgetPending(user);
	delete user;
}

void ADC::Hub::onPublicChat(ADC::HubUser* user, const char* line) {
	/* A user in State_Normal has sent its INF and so has a nick, but a service
	   or a caller reaching here another way need not have. */
	if (!line || !user->nick) return;

	/* Configured to keep none means nothing is recorded. Read the other way -
	   as a ring that never rotates - it is the chat log growing for as long as
	   the hub runs. */
	if (!maxHistory) return;

	Samurai::TimeStamp timestamp_obj;
	const std::string timestamp = timestamp_obj.getTime("%H:%M");
	const size_t chat_size = strlen(line) + timestamp.size() + strlen(user->nick) + 9;
	char* chat = static_cast<char*>(malloc(chat_size));
	snprintf(chat, chat_size, "[%s] <%s> %s", timestamp.c_str(), user->nick, line);

	if (history.size() >= maxHistory) {
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

		/*
		 * Choked at 'not High' rather than at 'Low'. Per the command table only
		 * SCH and RES are Low, so a user that had stopped reading was still
		 * handed every MSG, CTM, RCM and STA on the hub - which is to say a
		 * broadcast flood grew its queue without bound. High is what a user must
		 * have to make any sense of the hub at all: INF, SUP, PAS and DSC.
		 */
		const bool droppable = (cmd->getPriority() != ADC::Command::High);
		const bool feature_cast = cmd->isCommandFeature();

		/* kick() may be called below, and a deferred userAdd() may append while
		   this walks, so the recipients are decided up front. */
		std::vector<HubUser*> recipients;
		recipients.reserve(users.size());

		for (size_t n = 0; n < users.size(); n++) {
			if (users[n]->state != State_Normal) continue;

			if (droppable && users[n]->getSendQueueSize() > maxSendQ) continue;

			if (feature_cast) {
				bool wanted = true;
				std::vector<char*>::iterator it;
				for (it = cmd->features_include.begin();
					it != cmd->features_include.end() && wanted; it++)
					if (!users[n]->hasFeature((*it))) wanted = false;

				for (it = cmd->features_exclude.begin();
					it != cmd->features_exclude.end() && wanted; it++)
						if (users[n]->hasFeature((*it))) wanted = false;

				if (!wanted) continue;
			}

			recipients.push_back(users[n]);
		}

		std::vector<HubUser*> overflowed;
		for (size_t n = 0; n < recipients.size(); n++) {
			/* Refused outright means the queue is at its ceiling: the user is not
			   reading, and there is nothing to be gained by holding more for it. */
			if (!recipients[n]->send(cmd))
				overflowed.push_back(recipients[n]);
		}

		processing = false;

		/* Retired after the broadcast rather than during it: expire() broadcasts
		   in turn, and a broadcast nested inside this one would clear
		   'processing' halfway through it. */
		for (size_t n = 0; n < overflowed.size(); n++)
			expire(overflowed[n], "211", "Send queue exceeded");
	}
	cmd->decRef();
}


void ADC::Hub::expire(ADC::HubUser* user, const char* code, const char* reason) {
	QHUB("%s: Dropping '%s': %s", ADC::SID::toString(user->getSID()),
		user->nick ? user->nick : "(unidentified)", reason);

	const bool must_send_quit = (user->state == State_Normal);

	ADC::Command* sta = new ADC::Command(FOURCC('I','S','T','A'));
	sta->addArgument(code);
	sta->addArgument(reason);
	send(sta, user);
	kick(user);

	if (must_send_quit) {
		ADC::Command* quit = new ADC::Command(FOURCC('I','Q','U','I'));
		quit->addArgument(ADC::SID::toString(user->getSID()));
		quit->addArgument("MS", reason);
		send(quit);
	}
}


void ADC::Hub::kick(ADC::HubUser* user) {
	user->state = State_WaitDisconnect;

	if (user->getSendQueueSize() == 0 && !user->disconnecting) {
		/*
		 * Deferred rather than immediate: disconnect() reports EventDisconnected
		 * synchronously, which lands in onQuit() and deletes the user - while
		 * every caller here still holds it, and setINF() goes on to read the
		 * kicked user's SID. Anything still queued is left to EventCanWrite,
		 * which comes back here once it has drained, so the kick message itself
		 * still reaches the user.
		 */
		user->disconnecting = true;
		Samurai::postMessage(QuickDC::MsgHubUserDisconnect, 0, static_cast<size_t>(deferFor(user)), 0);
	}
}


size_t ADC::Hub::countLoggedIn() const {
	size_t count = 0;
	for (size_t n = 0; n < users.size(); n++)
		if (users[n]->state == State_Normal) count++;
	return count;
}

size_t ADC::Hub::countHandshaking() const {
	size_t count = 0;
	for (size_t n = 0; n < users.size(); n++)
		if (users[n]->state != State_Normal && users[n]->state != State_WaitDisconnect) count++;
	return count;
}

size_t ADC::Hub::countFromAddress(const Samurai::IO::Net::InetAddress& address) const {
	size_t count = 0;
	for (size_t n = 0; n < users.size(); n++) {
		const Samurai::IO::Net::InetAddress* other = users[n]->socket->getAddress();
		if (other && *other == address) count++;
	}
	return count;
}

bool ADC::Hub::admitConnection(const Samurai::IO::Net::Socket* socket) {
	if (!socket) return false;

	/*
	 * Room is left above the user limit for connections still in the login
	 * exchange, since an operator is only recognised as one after its INF and
	 * would otherwise never reach a full hub to be let in.
	 */
	if (users.size() >= static_cast<size_t>(maxUsers) + maxHandshake) {
		QHUB("Refusing a connection from %s: %zu connections already",
			peerAddress(socket).c_str(), users.size());
		return false;
	}

	/*
	 * Holding a slot costs the peer nothing until it has logged in, so the ones
	 * that have not are counted apart: that is the whole of the slow handshake,
	 * which otherwise fills the descriptor table for free.
	 */
	if (countHandshaking() >= maxHandshake) {
		QHUB("Refusing a connection from %s: %zu already in the login exchange",
			peerAddress(socket).c_str(), countHandshaking());
		return false;
	}

	/*
	 * An address we cannot read is refused rather than exempted. The per-address
	 * limit is what bounds one host opening connections in a loop, and skipping
	 * the check for a connection whose address is unknown made that the way
	 * around it - the one connection the limit never applied to. There is also
	 * nothing to attribute such a connection to afterwards.
	 */
	const Samurai::IO::Net::InetAddress* address = socket->getAddress();
	if (!address) {
		QHUB("Refusing a connection whose address cannot be read");
		return false;
	}

	if (countFromAddress(*address) >= maxPerAddress) {
		QHUB("Refusing a connection from %s: %zu connections from that address",
			address->toString().c_str(), countFromAddress(*address));
		return false;
	}

	return true;
}

void ADC::Hub::onConnected(std::shared_ptr<Samurai::IO::Net::Socket> socket) {
	if (!admitConnection(socket.get())) {
		/*
		 * Written straight to the socket, because refusing means no HubUser and
		 * so no command proxy to queue it on. The connection is not closed here:
		 * the caller is still holding it and goes on to use it, and it is dropped
		 * as soon as that last reference goes.
		 */
		static const char refused[] = "ISTA 211 Hub\\sis\\sfull\n";
		socket->write(refused, sizeof(refused) - 1);
		return;
	}

	ADC::HubUser* user = new ADC::HubUser(std::move(socket), this);
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
	if (!user) return;

	/*
	 * write() can fail hard, and the socket reports that through EventError
	 * before returning - which retires this very user. 'locked' makes onQuit()
	 * defer instead of deleting underneath us, and the user is looked up again
	 * afterwards rather than reused, since the deferred quit may already have
	 * run by the time control comes back here.
	 */
	user->locked = true;
	user->proxy->write();
	user->locked = false;

	user = lookupUser(socket);
	if (user && user->state == State_WaitDisconnect && user->getSendQueueSize() == 0) {
		kick(user);
	}
}

void ADC::Hub::EventDisconnected(const Samurai::IO::Net::Socket* socket)  {
	ADC::HubUser* user = lookupUser(socket);
	if (user) {
		onQuit(user);
	}
}

void ADC::Hub::EventError(const Samurai::IO::Net::Socket* socket, Samurai::IO::Net::SocketError error, const char* msg)
{
	ADC::HubUser* user = lookupUser(socket);
	if (!user) return;

	/*
	 * The detail is logged and goes no further. 'msg' is the C library's text for
	 * this one peer's errno, and the quit below is broadcast to every other user
	 * on the hub - so passing it on published one peer's local error conditions,
	 * and the host's operating system, to all of them. They are told a phrase of
	 * ours instead.
	 */
	QHUB("%s: Socket error %d: %s", ADC::SID::toString(user->sid),
		static_cast<int>(error), msg ? msg : "no detail given");
	(void) error;
	(void) msg;

	onQuit(user, ADC_STR_CONNECTION_LOST);
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

/*
 * A user only reaches State_Normal once its INF gave both a nick and a CID, so
 * the stored side of each of these is there to be compared; the name being
 * looked for is the side that may be missing, and no logged-in user answers to
 * nothing.
 */
ADC::HubUser* ADC::Hub::lookupUser(const char* nick) {
	const std::string_view wanted(nick ? nick : "");

	for (size_t n = 0; n < users.size(); n++) {
		if (users[n]->state == State_Normal && Samurai::Util::iequals(users[n]->nick, wanted)) {
			return users[n];
		}
	}
	return 0;
}

ADC::HubUser* ADC::Hub::lookupUserCID(const char* cid) {
	const std::string_view wanted(cid ? cid : "");

	for (size_t n = 0; n < users.size(); n++) {
		if (users[n]->state == State_Normal && Samurai::Util::iequals(users[n]->cid, wanted)) {
			return users[n];
		}
	}
	return 0;
}

void ADC::Hub::userAdd(HubUser* user) {
	if (processing) {
		Samurai::postMessage(QuickDC::MsgHubUserAppendUser, 0, static_cast<size_t>(deferFor(user)), 0);
		return;
	}

	socket_map.insert(std::pair<const Samurai::IO::Net::Socket*, HubUser*>(user->socket.get(), user));

	/*
	 * insert() keeps the entry that is already there, so a SID handed out twice
	 * would leave the second user missing from the map entirely - and since D and
	 * E messages are routed by target SID alone, its private messages would go to
	 * the first user instead. generateNextSid() is what makes this impossible;
	 * this is here so that a mistake there cannot pass silently.
	 */
	const bool unique_sid = sid_map.insert(std::pair<sid_t, HubUser*>(user->sid, user)).second;
	users.push_back(user);

	if (!unique_sid) {
		QHUB("%s: session ID already in use, dropping the connection.", ADC::SID::toString(user->sid));
		kick(user);
	}
}

void ADC::Hub::userRemove(HubUser* user) {
	if (processing) {
		Samurai::postMessage(QuickDC::MsgHubUserRemoveUser, 0, static_cast<size_t>(deferFor(user)), 0);
		return;
	}

	if (user) {
		std::map<const Samurai::IO::Net::Socket*, HubUser*>::iterator socket_it = socket_map.find(user->socket.get());
		if (socket_it != socket_map.end()) {
			socket_map.erase(socket_it);
		}
		/* Only if the entry is this user's: a SID that ended up shared would
		   otherwise have one user's departure unmap the other. */
		std::map<sid_t, HubUser*>::iterator sid_it = sid_map.find(user->sid);
		if (sid_it != sid_map.end() && (*sid_it).second == user) {
			sid_map.erase(sid_it);
		}
		/*
		 * NOTE: this loop used to be written as
		 *
		 *     for (user_it = users.begin(); *user_it; user_it++)
		 *
		 * dereferencing the iterator without ever comparing it against end().
		 * A user that is not in the vector - one whose deferred userAdd() has
		 * not been processed yet, say - therefore ran off the end reading
		 * whatever followed until it happened to find a zero, and erase() then
		 * memmoved an out of range interval. The same mistake is described at
		 * length in network/connectionmanager.cpp.
		 */
		for (std::vector<HubUser*>::iterator user_it = users.begin(); user_it != users.end(); user_it++) {
			if (*user_it == user) {
				users.erase(user_it);
				break;
			}
		}
	}
}

sid_t ADC::Hub::generateNextSid()
{
	/*
	 * The value that is returned is the value that was tested, and it is masked
	 * to the 20 bits a SID has - the same masking SID::toString() does - so that
	 * what the user is told its SID is, is what the hub has it under.
	 *
	 * Zero is skipped: SID::fromString() answers 0 for anything it cannot read,
	 * so a user holding it would be the one every malformed target SID resolves
	 * to.
	 */
	for (sid_t attempt = 0; attempt <= ADC::SID_MASK; attempt++) {
		const sid_t candidate = (++availSid) & ADC::SID_MASK;
		if (candidate && !lookupUser(candidate)) {
			availSid = candidate;
			return candidate;
		}
	}
	return 0;
}

/*
 * Nothing else ever looks at how long a connection has been quiet: the timestamp
 * is kept on every read and write, and this is what reads it. Without it a peer
 * that connects, says the four bytes the protocol probe needs and then nothing at
 * all, holds a descriptor for as long as the hub runs.
 */
void ADC::Hub::EventTimeout(Samurai::Timer*) {
	if (processing) return;

	/* Copied: expire() may broadcast, and a deferred userAdd() processed in the
	   meantime appends to 'users'. */
	const std::vector<HubUser*> snapshot(users);

	for (size_t n = 0; n < snapshot.size(); n++) {
		HubUser* user = snapshot[n];
		if (user->state == State_WaitDisconnect) continue;

		/* Two limits, because the two are not the same wait: a client that has
		   not finished identifying itself is doing something that takes seconds,
		   while one that has may legitimately sit and watch. */
		const bool identified = (user->state == State_Normal);
		const uint32_t limit = identified ? idleTimeout : handshakeTimeout;
		if (!limit) continue;

		if (user->getIdleTime() < static_cast<time_t>(limit)) continue;

		expire(user, "213", identified ? "Idle too long" : "Took too long to log in");
	}
}

bool ADC::Hub::EventMessage(const Samurai::Message* msg) {
	if (!processing) {
		/*
		 * The user is named by a ticket rather than carried as a pointer, so a
		 * user deleted between the post and here resolves to nothing instead of
		 * to freed memory. takePending() consumes the ticket either way.
		 */
		if (msg->getID() == QuickDC::MsgHubUserRemoveUser)
		{
			ADC::HubUser* user = takePending(static_cast<uint64_t>(msg->getArg1()));
			if (user) userRemove(user);
			return true;
		}

		if (msg->getID() == QuickDC::MsgHubUserAppendUser)
		{
			ADC::HubUser* user = takePending(static_cast<uint64_t>(msg->getArg1()));
			if (user) userAdd(user);
			return true;
		}

		if (msg->getID() == QuickDC::MsgHubUserDelayedQuit)
		{
			ADC::HubUser* user = takePending(static_cast<uint64_t>(msg->getArg1()));
			if (user) onQuit(user, "Delayed quit");
			return true;
		}

		if (msg->getID() == QuickDC::MsgHubUserDisconnect)
		{
			ADC::HubUser* user = takePending(static_cast<uint64_t>(msg->getArg1()));
			if (user)
			{
				user->disconnecting = false;
				/*
				 * Nothing up the stack is holding the user, so it is safe to let
				 * it go here - but the socket is held over the call: disconnect()
				 * reports EventDisconnected synchronously, which retires the
				 * user, and the user is the last owner of its socket, so the
				 * socket would be freed underneath the call running on it.
				 */
				std::shared_ptr<Samurai::IO::Net::Socket> socket = user->socket;
				socket->disconnect();
			}
			return true;
		}

	} else {
		QHUB("Parsing messages while processing...");
	}
	return false;
}
