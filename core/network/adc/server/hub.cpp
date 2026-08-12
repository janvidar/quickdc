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
#include "network/adc/server/hub.h"
#include "network/adc/server/service.h"
#include "network/adc/server/user.h"
#include "network/adc/server/claimcheck.h"
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
	ADC::LocalHub::User* user  = lookupUser(ADC::SID::fromString(cmd->getSourceID())); \
	ADC::LocalHub::User* user_sock = lookupUser(cmd->getSocket()); \
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

ADC::LocalHub::Hub::Hub() {
	welcome    = "This hub is running " PRODUCT " " VERSION " on " SYSTEM "/" CPU;
	hubname    = 0;
	hubdesc    = 0;
	readConfiguration();

	availSid = 0;

	acl = new ADC::LocalHub::ACL::Table();
	
	/* Start services */
	services = new ADC::LocalHub::ServiceMap(this);
	new ADC::LocalHub::SrvVersion(services);
	new ADC::LocalHub::SrvHelp(services);
	new ADC::LocalHub::SrvUptime(services);
	new ADC::LocalHub::SrvMotd(services);
	new ADC::LocalHub::SrvHistory(services);
	new ADC::LocalHub::SrvMyIP(services);
	
	processing = false;
	nextPendingTicket = 1;

	timer = new Samurai::Timer(this, ADC::LocalHub::HUB_SWEEP_INTERVAL, false);
}

uint64_t ADC::LocalHub::Hub::deferFor(ADC::LocalHub::User* user)
{
	const uint64_t ticket = nextPendingTicket++;
	pending.insert(std::pair<uint64_t, User*>(ticket, user));
	return ticket;
}

ADC::LocalHub::User* ADC::LocalHub::Hub::takePending(uint64_t ticket)
{
	std::map<uint64_t, User*>::iterator it = pending.find(ticket);
	if (it == pending.end()) return 0;

	User* user = (*it).second;
	pending.erase(it);
	return user;
}

void ADC::LocalHub::Hub::forgetPending(const ADC::LocalHub::User* user)
{
	std::map<uint64_t, User*>::iterator it = pending.begin();
	while (it != pending.end())
	{
		if ((*it).second == user) it = pending.erase(it);
		else it++;
	}
}

ADC::LocalHub::Hub::~Hub() {
	delete timer; timer = 0;
	delete acl;
	delete services;

	/*
	 * Same reasoning as the users below: a check in flight holds this hub and
	 * reports to it seconds later, so one left behind reports into freed memory.
	 */
	ADC::LocalHub::ClaimCheck::forget(this);

	/*
	 * Deleted, not merely unmapped. A user holds the last reference to its socket
	 * and the socket's event handler is this hub, so a user left behind here keeps
	 * a registered socket pointing at a hub that no longer exists - and the next
	 * readiness event on it is dispatched into freed memory.
	 */
	while (users.size()) {
		ADC::LocalHub::User* user = users.back();
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

void ADC::LocalHub::Hub::readConfiguration() {
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
	lingerTimeout    = static_cast<uint32_t>(std::clamp(
		config->getNumber("Linger timeout", static_cast<int>(HUB_LINGER_TIMEOUT)), 0, 86400));
	enabled    = config->getBool("Enabled", false);
	checkClaims = config->getBool("Check reachability claims", true);

	/* Choking a user is a hint that it is slow; this is the point at which it is
	   not reading at all, and holding anything more for it is pointless. */
	hardSendQ  = maxSendQ * 4;

	if (!enabled) {
		// FIXME: Kill all connections
	}
}

void ADC::LocalHub::Hub::onSUP(ADC::Command* cmd) {
	ADC::LocalHub::User* user = lookupUser(cmd->getSocket());
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

		/*
		 * Added here rather than in addSupportFlags(), which a client uses to
		 * introduce itself through the same branch: relaying NATT and being able to
		 * use it are different claims, and a hub that can pass DNAT and DRNT
		 * between two clients says nothing about whether its operator's own client
		 * could take part.
		 */
		c_sup->addArgument("AD", ADC_EXT_NATT);

		/*
		 * And PING for the same reason: it says this hub will answer a pinger asking
		 * what it is and who is on it, which is a thing only a hub can offer. A
		 * client that claimed it would be advertising a service it does not run.
		 */
		c_sup->addArgument("AD", ADC_EXT_PING);

		ADC::Command* c_sid = new ADC::Command(FOURCC('I','S','I','D'));
		c_sid->addArgument(ADC::SID::toString(user->sid));
		send(c_sup, user, true);
		send(c_sid, user, false);
		user->state = State_Identify;
	}
}

void ADC::LocalHub::Hub::onINF(ADC::Command* cmd) {
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
			/*
			 * Relayed whatever it changed. This asked for more than one field, so
			 * an update that changed exactly one thing was accumulated here and
			 * told to nobody - a share size, a description, or an 'I40.0.0.0'
			 * asking what address the hub sees, which is how a client behind a NAT
			 * finds out.
			 *
			 * Unconditional because setINF() answers with the number of fields it
			 * took, so an update that changed nothing has already failed the test
			 * above and does not reach here.
			 */
			send(cmd);
		}
	} else {
		
		if (user->state == State_Identify) {
			kick(user);
		}
	}
}

void ADC::LocalHub::Hub::onRES(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::LocalHub::User* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (target && target->state == State_Normal) {
		send(cmd, target);
	}
}

void ADC::LocalHub::Hub::onMSG(ADC::Command* cmd) {
	CHECK_SID_OK();
	
	ADC::LocalHub::User* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
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

void ADC::LocalHub::Hub::onSCH(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::LocalHub::User* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
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

void ADC::LocalHub::Hub::onSTA(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::LocalHub::User* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (user->state == State_Normal && target && target->state == State_Normal) {
		send(cmd, target);
	}
}

void ADC::LocalHub::Hub::onCTM(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::LocalHub::User* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (user->state == State_Normal && target && target->state == State_Normal) {
		send(cmd, target);

		/*
		 * The one place a client names the TCP port it expects peers to arrive on.
		 * INF's U4 is the UDP port and says nothing about this one, so a CTM is the
		 * hub's only occasion to check what TCP4 asserts - and the sender has just
		 * told a peer to connect there, so it is the moment the claim is being acted
		 * on rather than merely advertised.
		 */
		if (checkClaims && user->hasFeature(ADC_EXT_TCP4)) {
			const int port = Samurai::Util::Convert::to_int32(cmd->getArgument(1));
			if (port > 0 && port <= 65535)
				ADC::LocalHub::ClaimCheck::start(this, user, cmd->getArgument(0),
					static_cast<uint16_t>(port));
		}
	}
}

void ADC::LocalHub::Hub::onRCM(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::LocalHub::User* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (user->state == State_Normal && target && target->state == State_Normal) {
		send(cmd, target);
	}
}


/*
 * NATT, which two clients that can neither of them be connected to use to reach
 * each other: one names the port it will dial from, the other answers with its
 * own, and both dial at once. Relayed as CTM and RCM are, because the hub is the
 * only path between them.
 *
 * The port belongs to the client that named it and describes a mapping only its
 * own NAT knows about, so unlike I4 there is nothing here the hub could verify.
 * What it can do is what it already does for the address: forward it only between
 * two users who are both logged in.
 */
void ADC::LocalHub::Hub::onNAT(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::LocalHub::User* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (user->state == State_Normal && target && target->state == State_Normal) {
		send(cmd, target);
	}
}

void ADC::LocalHub::Hub::onRNT(ADC::Command* cmd) {
	CHECK_SID_OK();
	ADC::LocalHub::User* target = lookupUser(ADC::SID::fromString(cmd->getTargetID()));
	if (user->state == State_Normal && target && target->state == State_Normal) {
		send(cmd, target);
	}
}

void ADC::LocalHub::Hub::onDSC(ADC::Command* cmd) {
	CHECK_SID_OK();
	if (user->credentials == Cred_Operator || user->credentials == Cred_Admin) {
		ADC::LocalHub::User* target = lookupUser(ADC::SID::fromString(cmd->getArgument(0)));
		
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

void ADC::LocalHub::Hub::onPAS(ADC::Command*) {
	// FIXME: Handle password authentication
}

void ADC::LocalHub::Hub::onWelcome(ADC::LocalHub::User* user) {
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

void ADC::LocalHub::Hub::onQuit(ADC::LocalHub::User* user, const char* msg) {
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

void ADC::LocalHub::Hub::onPublicChat(ADC::LocalHub::User* user, const char* line) {
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

void ADC::LocalHub::Hub::send(ADC::Command* cmd, User* user, bool more) {
	cmd->incRef();

	if (user) {
		// Send message directly to one user only
		bool echo = cmd->isCommandEcho();
		user->send(cmd, more);
		if (echo) {
			// If message is an echo message, the sender also want a copy of the message.
			ADC::LocalHub::User* source = lookupUser(ADC::SID::fromString(cmd->getSourceID()));
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
		std::vector<User*> recipients;
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

		std::vector<User*> overflowed;
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


void ADC::LocalHub::Hub::expire(ADC::LocalHub::User* user, const char* code, const char* reason) {
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


void ADC::LocalHub::Hub::drop(ADC::LocalHub::User* user) {
	/*
	 * Deferred rather than immediate: disconnect() reports EventDisconnected
	 * synchronously, which lands in onQuit() and deletes the user - while every
	 * caller here still holds it, and setINF() goes on to read the kicked user's
	 * SID.
	 */
	if (user->disconnecting) return;

	user->disconnecting = true;
	Samurai::postMessage(QuickDC::MsgHubUserDisconnect, 0, static_cast<size_t>(deferFor(user)), 0);
}

void ADC::LocalHub::Hub::kick(ADC::LocalHub::User* user) {
	user->state = State_WaitDisconnect;

	/*
	 * Anything still queued is left to EventCanWrite, which comes back here once
	 * it has drained, so the kick message itself still reaches the user - but only
	 * for lingerTimeout, since that event depends on the peer reading and a peer
	 * that has stopped would otherwise hold the connection for good. The sweep in
	 * EventTimeout() is what ends the wait.
	 */
	if (user->getSendQueueSize() == 0)
		drop(user);
}


size_t ADC::LocalHub::Hub::countLoggedIn() const {
	size_t count = 0;
	for (size_t n = 0; n < users.size(); n++)
		if (users[n]->state == State_Normal) count++;
	return count;
}

size_t ADC::LocalHub::Hub::countHandshaking() const {
	size_t count = 0;
	for (size_t n = 0; n < users.size(); n++)
		if (users[n]->state != State_Normal && users[n]->state != State_WaitDisconnect) count++;
	return count;
}

size_t ADC::LocalHub::Hub::countFromAddress(const Samurai::IO::Net::InetAddress& address) const {
	size_t count = 0;
	for (size_t n = 0; n < users.size(); n++) {
		const Samurai::IO::Net::InetAddress* other = users[n]->socket->getAddress();
		if (other && *other == address) count++;
	}
	return count;
}

bool ADC::LocalHub::Hub::admitConnection(const Samurai::IO::Net::Socket* socket) {
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

void ADC::LocalHub::Hub::onConnected(std::shared_ptr<Samurai::IO::Net::Socket> socket) {
	if (!admitConnection(socket.get())) {
		/*
		 * Written straight to the socket, because refusing means no User and
		 * so no command proxy to queue it on. The connection is not closed here:
		 * the caller is still holding it and goes on to use it, and it is dropped
		 * as soon as that last reference goes.
		 */
		static const char refused[] = "ISTA 211 Hub\\sis\\sfull\n";
		socket->write(refused, sizeof(refused) - 1);
		return;
	}

	ADC::LocalHub::User* user = new ADC::LocalHub::User(std::move(socket), this);
	userAdd(user);
}


void ADC::LocalHub::Hub::EventHostLookup(const Samurai::IO::Net::Socket*) { }
void ADC::LocalHub::Hub::EventHostFound(const Samurai::IO::Net::Socket*)  { }
void ADC::LocalHub::Hub::EventConnecting(const Samurai::IO::Net::Socket*) { }
void ADC::LocalHub::Hub::EventTimeout(const Samurai::IO::Net::Socket*)    { }
void ADC::LocalHub::Hub::EventConnected(const Samurai::IO::Net::Socket*)  { }

void ADC::LocalHub::Hub::EventDataAvailable(const Samurai::IO::Net::Socket* socket) {
	ADC::LocalHub::User* user = lookupUser(socket);
	if (user) {
		user->read();
	}
}

void ADC::LocalHub::Hub::EventCanWrite(const Samurai::IO::Net::Socket* socket) {
	ADC::LocalHub::User* user = lookupUser(socket);
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

void ADC::LocalHub::Hub::EventDisconnected(const Samurai::IO::Net::Socket* socket)  {
	ADC::LocalHub::User* user = lookupUser(socket);
	if (user) {
		onQuit(user);
	}
}

void ADC::LocalHub::Hub::EventError(const Samurai::IO::Net::Socket* socket, Samurai::IO::Net::SocketError error, const char* msg)
{
	ADC::LocalHub::User* user = lookupUser(socket);
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

ADC::LocalHub::User* ADC::LocalHub::Hub::lookupUser(const Samurai::IO::Net::Socket* socket) {
	User* user = 0;
	std::map<const Samurai::IO::Net::Socket*, User*>::iterator socket_it = socket_map.find(socket);
	if (socket_it != socket_map.end()) {
		user = (*socket_it).second;
	}
	return user;
}

ADC::LocalHub::User* ADC::LocalHub::Hub::lookupUser(sid_t sid) {
	User* user = 0;
	std::map<sid_t, User*>::iterator sid_it = sid_map.find(sid);
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
ADC::LocalHub::User* ADC::LocalHub::Hub::lookupUser(const char* nick) {
	const std::string_view wanted(nick ? nick : "");

	for (size_t n = 0; n < users.size(); n++) {
		if (users[n]->state == State_Normal && Samurai::Util::iequals(users[n]->nick, wanted)) {
			return users[n];
		}
	}
	return 0;
}

ADC::LocalHub::User* ADC::LocalHub::Hub::lookupUserCID(const char* cid) {
	const std::string_view wanted(cid ? cid : "");

	for (size_t n = 0; n < users.size(); n++) {
		if (users[n]->state == State_Normal && Samurai::Util::iequals(users[n]->cid, wanted)) {
			return users[n];
		}
	}
	return 0;
}

void ADC::LocalHub::Hub::userAdd(User* user) {
	if (processing) {
		Samurai::postMessage(QuickDC::MsgHubUserAppendUser, 0, static_cast<size_t>(deferFor(user)), 0);
		return;
	}

	socket_map.insert(std::pair<const Samurai::IO::Net::Socket*, User*>(user->socket.get(), user));

	/*
	 * insert() keeps the entry that is already there, so a SID handed out twice
	 * would leave the second user missing from the map entirely - and since D and
	 * E messages are routed by target SID alone, its private messages would go to
	 * the first user instead. generateNextSid() is what makes this impossible;
	 * this is here so that a mistake there cannot pass silently.
	 */
	const bool unique_sid = sid_map.insert(std::pair<sid_t, User*>(user->sid, user)).second;
	users.push_back(user);

	if (!unique_sid) {
		QHUB("%s: session ID already in use, dropping the connection.", ADC::SID::toString(user->sid));
		kick(user);
	}
}

void ADC::LocalHub::Hub::userRemove(User* user) {
	if (processing) {
		Samurai::postMessage(QuickDC::MsgHubUserRemoveUser, 0, static_cast<size_t>(deferFor(user)), 0);
		return;
	}

	if (user) {
		std::map<const Samurai::IO::Net::Socket*, User*>::iterator socket_it = socket_map.find(user->socket.get());
		if (socket_it != socket_map.end()) {
			socket_map.erase(socket_it);
		}
		/* Only if the entry is this user's: a SID that ended up shared would
		   otherwise have one user's departure unmap the other. */
		std::map<sid_t, User*>::iterator sid_it = sid_map.find(user->sid);
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
		for (std::vector<User*>::iterator user_it = users.begin(); user_it != users.end(); user_it++) {
			if (*user_it == user) {
				users.erase(user_it);
				break;
			}
		}
	}
}

sid_t ADC::LocalHub::Hub::generateNextSid()
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
	for (sid_t attempt = 0; attempt <= ADC::LocalHub::SID_MASK; attempt++) {
		const sid_t candidate = (++availSid) & ADC::LocalHub::SID_MASK;
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
void ADC::LocalHub::Hub::EventTimeout(Samurai::Timer*) {
	if (processing) return;

	/* Copied: expire() may broadcast, and a deferred userAdd() processed in the
	   meantime appends to 'users'. */
	const std::vector<User*> snapshot(users);

	for (size_t n = 0; n < snapshot.size(); n++) {
		User* user = snapshot[n];

		/*
		 * A user on its way out is waiting for room to send the rest of its kick
		 * into, which is not something it can be asked to hurry along - and if the
		 * peer has stopped reading altogether, room never appears and the wait is
		 * forever. This is the end of it: the connection goes and whatever is still
		 * queued goes with it. Skipping the state outright is what left a kicked
		 * user in the hub's tables for the life of the process.
		 */
		if (user->state == State_WaitDisconnect) {
			if (lingerTimeout && user->getIdleTime() >= static_cast<time_t>(lingerTimeout))
				drop(user);
			continue;
		}

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

bool ADC::LocalHub::Hub::EventMessage(const Samurai::Message* msg) {
	const size_t id = msg->getID();

	const bool ours = (id == QuickDC::MsgHubUserRemoveUser
	                || id == QuickDC::MsgHubUserAppendUser
	                || id == QuickDC::MsgHubUserDelayedQuit
	                || id == QuickDC::MsgHubUserDisconnect);

	if (!ours) return false;

	/*
	 * Put back rather than dropped.
	 *
	 * All four of these mutate the tables a broadcast walks, which is why they
	 * are not run while one is in progress. Returning without running them lost
	 * them: the ticket stays in 'pending' and nothing else ever posts it again, so
	 * a user queued for removal was never removed - and one queued for
	 * disconnection sat in State_WaitDisconnect for the life of the process, with
	 * its socket readable and nothing willing to read it.
	 *
	 * Reposting is safe and terminates: a message posted while the handler is busy
	 * is carried over to its next pass, and that pass is not inside a broadcast.
	 */
	if (processing) {
		QHUB("A hub message arrived during a broadcast; deferring it to the next pass");
		Samurai::postMessage(id, msg->getData(), msg->getArg1(), msg->getArg2());
		return true;
	}

	/*
	 * The user is named by a ticket rather than carried as a pointer, so a user
	 * deleted between the post and here resolves to nothing instead of to freed
	 * memory. takePending() consumes the ticket either way.
	 */
	if (id == QuickDC::MsgHubUserRemoveUser)
	{
		ADC::LocalHub::User* user = takePending(static_cast<uint64_t>(msg->getArg1()));
		if (user) userRemove(user);
		return true;
	}

	if (id == QuickDC::MsgHubUserAppendUser)
	{
		ADC::LocalHub::User* user = takePending(static_cast<uint64_t>(msg->getArg1()));
		if (user) userAdd(user);
		return true;
	}

	if (id == QuickDC::MsgHubUserDelayedQuit)
	{
		ADC::LocalHub::User* user = takePending(static_cast<uint64_t>(msg->getArg1()));
		if (user) onQuit(user, "Delayed quit");
		return true;
	}

	if (id == QuickDC::MsgHubUserDisconnect)
	{
		ADC::LocalHub::User* user = takePending(static_cast<uint64_t>(msg->getArg1()));
		if (user)
		{
			user->disconnecting = false;

			/*
			 * Nothing up the stack is holding the user, so it is safe to let it go
			 * here - but the socket is held over the call: disconnect() retires the
			 * user through EventDisconnected, and the user is the last owner of its
			 * socket, so the socket would be freed underneath the call running on it.
			 */
			std::shared_ptr<Samurai::IO::Net::Socket> socket = user->socket;
			socket->disconnect();
		}
		return true;
	}

	return false;
}
