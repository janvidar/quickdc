/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"

#include <api/core.h>

#include <samurai/eventloop.h>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>
#include <string.h>
#include <string>
#include <string_view>

#include "config/preferences.h"
#include "network/adc/adcbbs.h"
#include "network/adc/server/acl.h"
#include "network/adc/server/boards.h"
#include "network/adc/server/hub.h"
#include "network/adc/server/service.h"
#include "network/adc/server/user.h"
#include "network/adc/server/claimcheck.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/adccommands.h"
#include "network/adc/cid.h"
#include "network/adc/parser.h"
#include "config/settings.h"

static_assert(QuickDC::Settings::HubLingerTimeout.fallback == ADC::LocalHub::HUB_LINGER_TIMEOUT,
              "the setting falls back to the hub's own linger");
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

ADC::LocalHub::Hub::Hub(Samurai::EventLoop& loop_, QuickDC::Preferences& config_,
                        const std::string& directory_)
	: Samurai::MessageListener(loop_)
	, eventLoop(loop_)
	, config(config_)
	, home(directory_)
	, startup(time(0))
{
	welcome    = "This hub is running " PRODUCT " " VERSION " on " SYSTEM "/" CPU;
	hubname    = 0;
	hubdesc    = 0;

	/* Before the configuration is read, because that is where the store is made
	   and it decides whether there is one to make. */
	boards     = 0;
	readConfiguration();

	availSid = 0;

	acl = new ADC::LocalHub::ACL::Table((home + "/acl.xml").c_str());
	
	/* Start services */
	services = new ADC::LocalHub::ServiceMap(this);
	new ADC::LocalHub::SrvVersion(services);
	new ADC::LocalHub::SrvHelp(services);
	new ADC::LocalHub::SrvUptime(services);
	new ADC::LocalHub::SrvMotd(services);
	new ADC::LocalHub::SrvHistory(services);
	new ADC::LocalHub::SrvMyIP(services);
	
	nextPendingTicket = 1;

	timer = new Samurai::Timer(eventLoop, this, ADC::LocalHub::HUB_SWEEP_INTERVAL, false);
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
	delete boards; boards = 0;

	/*
	 * Same reasoning as the users below: a check in flight holds this hub and
	 * reports to it seconds later, so one left behind reports into freed memory.
	 */
	claimChecks.clear();

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
	QuickDC::Preferences* config = &this->config;
	namespace S = QuickDC::Settings;
	
	free(hubname);
	free(hubdesc);
	
	hubname    = strdup(S::read(*config, S::HubName).c_str());
	hubdesc    = strdup(S::read(*config, S::HubDescription).c_str());

	/*
	 * Every one of these is clamped, because each is an int from a file the hub
	 * does not write and is used as an unsigned bound. A configured 0 for the
	 * history turned 'history.size() > maxHistory-1' into a comparison against
	 * 0xFFFFFFFF, so the ring never rotated and the chat log grew for as long as
	 * the hub ran.
	 */
	maxUsers   = static_cast<uint32_t>(std::clamp(S::read(*config, S::HubMaxUsers), 1, 65536));
	maxHistory = static_cast<uint32_t>(std::clamp(S::read(*config, S::HubMaxHistory), 0, 65536));
	maxSendQ   = static_cast<uint32_t>(std::clamp(S::read(*config, S::HubMaxSendQueue),
	                                              4 * 1024, 16 * 1024 * 1024));
	maxPerAddress = static_cast<uint32_t>(std::clamp(S::read(*config, S::HubMaxPerAddress), 1, 65536));
	maxHandshake  = static_cast<uint32_t>(std::clamp(S::read(*config, S::HubMaxHandshakes), 1, 65536));
	handshakeTimeout = static_cast<uint32_t>(std::clamp(S::read(*config, S::HubHandshakeTimeout), 0, 86400));
	idleTimeout      = static_cast<uint32_t>(std::clamp(S::read(*config, S::HubIdleTimeout), 0, 86400));
	lingerTimeout    = static_cast<uint32_t>(std::clamp(
		S::read(*config, S::HubLingerTimeout), 0, 86400));
	enabled    = S::read(*config, S::HubEnabled);
	checkClaims = S::read(*config, S::HubCheckClaims);

	/* Choking a user is a hint that it is slow; this is the point at which it is
	   not reading at all, and holding anything more for it is pointless. */
	hardSendQ  = maxSendQ * 4;

	/*
	 * The bulletin board indexes, one file per board.
	 *
	 * Made once and not remade on a re-read: the store holds the entries every
	 * subscriber has been sent and the highest timestamp handed out on each board,
	 * and a new one would read those back off disk and could hand a timestamp out
	 * twice. Moving the directory therefore takes a restart, which is said here
	 * rather than left to be discovered.
	 */
	if (!boards)
		boards = new ADC::LocalHub::BoardStore(config->peekString(
			S::HubBulletinDirectory.group, S::HubBulletinDirectory.key, home + "/bbs"));

	/*
	 * Taken before the boards are re-read, because a board that has gone owes
	 * every session that was following it a descriptor saying so - and after the
	 * re-read there is nothing left to say it about.
	 */
	const std::vector<std::string> before = boards->names();

	config->setGroup("Local hub");
	boards->readConfiguration(config);

	for (const std::string& board : before) {
		if (boards->exists(board)) announceBoard(board);
		else announceBoardRemoved(board);
	}

	for (const std::string& board : boards->names())
		if (std::find(before.begin(), before.end(), board) == before.end())
			announceBoard(board);
}

void ADC::LocalHub::Hub::closeEveryConnection() {
	const std::vector<User*> snapshot(users);

	for (User* user : snapshot) {
		if (user->state == State_WaitDisconnect) continue;

		expire(user, "212", "This hub is disabled");
	}
}

void ADC::LocalHub::Hub::onSUP(ADC::Command* cmd) {
	ADC::LocalHub::User* user = lookupUser(cmd->getSocket());
	if (!user) return;

	/*
	 * What the session offered, kept per session.
	 *
	 * Not the features of an INF: 'SU' is what this client wants the others on the
	 * hub told, which is why the hub relays it and casts searches by it, while a
	 * SUP says what this session may be asked for and is addressed to the hub
	 * alone. A board descriptor goes only to a session that named BBS0 here, and
	 * BBS0 is deliberately not an 'SU' flag - reading boards off a hub is no
	 * concern of any other client.
	 *
	 * Read whether or not this is the SUP that begins the handshake, because a
	 * client is free to send another one later to add or withdraw a name, and what
	 * it supports is whatever it last said.
	 */
	for (size_t n = 0; cmd->getArgument(n); n++) {
		const char* arg = cmd->getArgument(n);
		if (strlen(arg) != 6) continue;

		if (arg[0] == 'A' && arg[1] == 'D')
			user->addExtension(&arg[2]);
		else if (arg[0] == 'R' && arg[1] == 'M')
			user->removeExtension(&arg[2]);
	}

	/*
	 * A session that has taken BBS0 back is following nothing. The extension has a
	 * hub withdrawing the feature cancel every subscription on the connection, and
	 * a client withdrawing it has said the same thing from the other end: it will
	 * not read what arrives, and an index entry sent to a client that has stopped
	 * parsing them is at best ignored.
	 */
	if (!user->supportsExtension(ADC_EXT_BULLETIN))
		user->getSubscriptions().clear();

	/*
	 * And a session that has offered it after logging in is told what there is to
	 * read, which it would otherwise never learn: the descriptors go out on entry to
	 * NORMAL and this session was not asking for them then. Only the boards it has
	 * not already been told about, so that an offer repeated is not a burst repeated.
	 */
	else if (user->state == State_Normal)
		sendBoardDescriptors(user);

	if (user->state == State_Protocol) {
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

		/*
		 * And the bulletin boards, when there are any and when this hub has just
		 * said it speaks TIGR.
		 *
		 * The hub's announcement is what permits a client to send BBL or BBP at
		 * all, so a hub with no usable index, or none configured, must not make it:
		 * everything a client then sent could only be refused. The TIGR condition
		 * is the extension's own - every identifier in BBS0 is a tiger tree root
		 * and a client without TIGR cannot fetch a post body - and it is asked of
		 * the command rather than assumed, so that the two claims cannot drift
		 * apart if addSupportFlags() ever stops making one of them.
		 */
		if (hasBoards() && c_sup->haveArgument("AD" ADC_HASH_TIGER, 6))
			c_sup->addArgument("AD", ADC_EXT_BULLETIN);

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

				/*
				 * After the session's own INF has gone back to it, and not with the
				 * rest of the welcome: that echo is what tells a client its login
				 * finished, and a client is entitled to refuse a board descriptor
				 * that arrives before it - a hub putting a board's title in front of
				 * a session it has not yet accepted is exactly what such a client is
				 * guarding against. The extension asks for these after the INF
				 * exchange, and the exchange is not over until this has been sent.
				 */
				sendBoardDescriptors(user);

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


/* ------------------------------------------------------------------------- */
/* Bulletin boards                                                           */
/* ------------------------------------------------------------------------- */

/*
 * The hub holds an index and no bytes.
 *
 * Which board a post is on, when it arrived and what nickname its author held are
 * the hub's to say and are said in an index entry; the author, the parent, the
 * subject and the content are the author's and are said in the document, which
 * travels between clients and never through here. That split is why the fields a
 * client sends in a BBP are so few, and why four of the ones it might send anyway
 * - ID, NI, TH and TS - are discarded rather than refused: they are not the
 * client's to state, in the way PD is not part of the INF the hub relays.
 *
 * Nothing below trusts PE for anything. It says what to offer a user, and every
 * operation is re-checked as it arrives; a client is entitled to be refused
 * something its descriptor appeared to allow.
 */

/** Severity 1 and a two digit code, which is the whole of an ADC status code. */
static std::string statusCode(int code)
{
	char text[8] = {0,};
	snprintf(text, sizeof(text), "1%02d", code % 100);
	return std::string(text);
}

/**
 * Whether 'text' is a number the hub can read.
 *
 * Nineteen digits at the most, which is one short of what a 64 bit count can hold
 * and therefore the longest that cannot wrap. A wrapping size would be the
 * interesting one: a declared length of 2^64 + 1 read as 1 would pass a board's
 * limit and be recorded as a post of one byte.
 */
static bool isNumber(const char* text)
{
	if (!text || !*text) return false;

	const std::string_view value(text);
	if (value.size() > 19) return false;

	for (const char c : value)
		if (c < '0' || c > '9') return false;

	return true;
}

static uint64_t toNumber(const char* text)
{
	uint64_t value = 0;
	for (const char* at = text; at && *at; at++)
		value = (value * 10) + static_cast<uint64_t>(*at - '0');

	return value;
}

/** Whether a command carries exactly 'RM1', which is the only value defined. */
static bool isRemoval(ADC::Command* cmd)
{
	const char* flag = cmd->getArgument("RM", 2);
	return flag && std::string_view(flag) == "1";
}

/**
 * Whether a command carries an RM this version does not define.
 *
 * Every other value of the field is reserved, and reserved is refused rather than
 * ignored: a client sending RM2 has asked for something, and treating it as a
 * post - which is what a flag read as absent would do - would carry out the one
 * thing it certainly did not ask for.
 */
static bool hasReservedRemoval(ADC::Command* cmd)
{
	const char* flag = cmd->getArgument("RM", 2);
	return flag && std::string_view(flag) != "1";
}

bool ADC::LocalHub::Hub::sendOwned(ADC::Command* cmd, ADC::LocalHub::User* user, bool more)
{
	cmd->incRef();

	/*
	 * Queued for the one session directly rather than through the send() that also
	 * handles an echo, because none of these is one: every command the boards
	 * compose is an 'I' message addressed to a single recipient, and this way the
	 * queue's answer comes back - which a replay needs, having a great deal more to
	 * say and a reason to stop saying it.
	 */
	const bool queued = user ? user->send(cmd, more) : false;

	cmd->decRef();
	if (cmd->canDelete()) delete cmd;

	return queued;
}

bool ADC::LocalHub::Hub::hasBoards() const
{
	return boards && boards->isUsable() && boards->count() != 0;
}

void ADC::LocalHub::Hub::refuseBulletin(ADC::LocalHub::User* user, int code,
	const char* reason, const char* fourcc, const char* board, const char* tth,
	const char* field, const std::string& value)
{
	ADC::Command* sta = new ADC::Command(FOURCC('I','S','T','A'));
	sta->addArgument(statusCode(code).c_str());
	sta->addArgument(reason);
	sta->addArgument("FC", fourcc);

	/* The hash of the post, or the name of the board, because this extension has
	   no token: naming one of them is how a client matches a refusal to what it
	   asked for. Both where both are known. */
	if (tth) sta->addArgument("TR", tth);
	if (board) sta->addArgument("BD", board);
	if (field) sta->addArgument(field, value);

	sendOwned(sta, user);
}

void ADC::LocalHub::Hub::sendBoardDescriptors(ADC::LocalHub::User* user)
{
	if (!hasBoards()) return;

	/* Only a session that named the extension in its SUP. What a client wants
	   other clients told is a different question and is not asked here. */
	if (!user->supportsExtension(ADC_EXT_BULLETIN)) return;

	const std::vector<std::string> names = boards->names();

	for (const std::string& name : names) {
		/*
		 * Only boards this session has not been told about, so that a client
		 * offering the extension over and over is not a way to ask for a burst of
		 * one line per board for the price of one short command. A board whose
		 * fields have changed is re-sent by announceBoard(), which is the path that
		 * exists for saying something new.
		 */
		if (user->getSubscriptions().wasDescribed(name)) continue;

		QuickDC::Board board;
		if (!boards->describe(name, user->getCredentials(), board)) continue;

		/* No descriptor at all where the session may not subscribe. */
		if (!board.may(QuickDC::Subscribe)) continue;

		user->getSubscriptions().noteDescribed(name);
		sendOwned(writeBoard(board), user);
	}
}

void ADC::LocalHub::Hub::announceBoard(const std::string& board)
{
	if (!boards) return;

	/*
	 * Held over the walk, as a broadcast holds it: writing to a user can fail hard,
	 * the socket reports that before the write returns, and the user is retired on
	 * the spot - which would free an entry of the very vector being walked. The flag
	 * is what makes onQuit() defer instead. The one user that does not need it is
	 * the one whose command is being processed, which is locked for the same reason.
	 *
	 * Restored rather than cleared, here and in the two below, so that one of these
	 * called from inside something that had already set it cannot hand back a hub
	 * that is no longer protecting the walk it is in the middle of.
	 */
	const bool walking = processing;
	processing = true;

	for (size_t n = 0; n < users.size(); n++) {
		ADC::LocalHub::User* user = users[n];
		if (user->state != State_Normal) continue;
		if (!user->supportsExtension(ADC_EXT_BULLETIN)) continue;

		QuickDC::Board described;
		if (!boards->describe(board, user->getCredentials(), described)) continue;

		/*
		 * A session that may no longer subscribe is told nothing and stops
		 * following the board. Told nothing rather than told RM1, which would say
		 * the board had ceased to exist - it has not, and saying so to a session
		 * that has merely been demoted would have it drop a board that is still
		 * there for everybody else.
		 */
		if (!described.may(QuickDC::Subscribe)) {
			user->getSubscriptions().cancel(board);
			continue;
		}

		user->getSubscriptions().noteDescribed(board);
		sendOwned(writeBoard(described), user);
	}

	processing = walking;
}

void ADC::LocalHub::Hub::announceBoardRemoved(const std::string& board)
{
	QuickDC::Board gone;
	gone.name = board;
	gone.removed = true;

	/* As above: a user retired by a failing write must not be freed under this. */
	const bool walking = processing;
	processing = true;

	for (size_t n = 0; n < users.size(); n++) {
		ADC::LocalHub::User* user = users[n];
		if (user->state != State_Normal) continue;

		/* Only the sessions that were told the board was there, which is exactly
		   the set holding it now. */
		if (!user->getSubscriptions().wasDescribed(board)) continue;

		user->getSubscriptions().forget(board);
		sendOwned(writeBoard(gone), user);
	}

	processing = walking;
}

void ADC::LocalHub::Hub::sendEntry(const QuickDC::BoardEntry& entry,
	ADC::LocalHub::User* author)
{
	/* One command for every recipient, and a reference held across the loop: a
	   recipient's queue takes its own, and the last one out frees it. */
	ADC::Command* cmd = writeEntry(entry);
	cmd->incRef();

	/* And as in announceBoard(): a subscriber whose write fails is retired, and this
	   is what stops it being freed out of the vector being walked. */
	const bool walking = processing;
	processing = true;

	for (size_t n = 0; n < users.size(); n++) {
		ADC::LocalHub::User* user = users[n];
		if (user->state != State_Normal) continue;

		ADC::LocalHub::Subscriptions& following = user->getSubscriptions();
		const bool subscribed = following.has(entry.board);

		/*
		 * The submitting session receives the entry whether it follows the board or
		 * not. There is no success status message in this extension - the entry is
		 * the acknowledgement - so a session that posted without subscribing would
		 * otherwise be told nothing at all, and could not tell acceptance from a
		 * refusal that went missing.
		 */
		if (!subscribed && user != author) continue;

		/* The cursor follows what was queued and not what was attempted: a session
		   whose queue is at its ceiling has not been sent this entry, and a cursor
		   that moved past it would be a gap nothing could close. */
		if (user->send(cmd))
			following.advance(entry.board, entry.timestamp);
	}

	processing = walking;

	cmd->decRef();
	if (cmd->canDelete()) delete cmd;
}

void ADC::LocalHub::Hub::onBBL(ADC::Command* cmd) {
	CHECK_SID_OK();
	if (user->state != State_Normal) return;

	if (!hasBoards()) {
		/*
		 * This hub never announced the feature, so a client sending BBL is one that
		 * did not read the answer to its SUP. 45 is the feature error and carries
		 * FC, which says which command was not available rather than inventing a
		 * bulletin board condition for a hub that has no bulletin boards.
		 */
		refuseBulletin(user, ADC_CODE_PROTOCOL_FEATURE_ERROR,
			"This hub has no bulletin boards", "BBL", 0, 0);
		return;
	}

	ADC::LocalHub::Subscriptions& following = user->getSubscriptions();

	const char* raw_board = cmd->getArgument("BD", 2);
	const std::string named(raw_board ? raw_board : "");
	const bool board_ok = ADC::isValidBoardName(named);

	/* Echoed back only where it is a board name at all: a refusal repeats what it
	   was sent, and what it was sent is remote text of any length. */
	const char* board = board_ok ? named.c_str() : 0;

	const char* raw_tth = cmd->getArgument("TR", 2);
	const std::string wanted(raw_tth ? raw_tth : "");
	const char* tth = ADC::isValidTTH(wanted) ? wanted.c_str() : 0;

	/*
	 * Before the command is read, since a flood is answered rather than parsed. A
	 * subscription is a standing instruction to send data obtained with one short
	 * command, so the command itself is what has to be bounded.
	 */
	if (!following.requests.spend()) {
		refuseBulletin(user, ADC_CODE_BULLETIN_ERROR, "Too many board requests",
			"BBL", board, tth, "TL", std::to_string(following.requests.retryAfter()));
		return;
	}

	if (!raw_board) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED, "No board named",
			"BBL", 0, tth, "FM", "BD");
		return;
	}

	if (!board_ok) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED, "Not a board name",
			"BBL", 0, tth, "FB", "BD");
		return;
	}

	const char* raw_from = cmd->getArgument("TS", 2);

	/* A question about one entry and a subscription from a point in time are two
	   different commands, and one carrying both is neither. */
	if (raw_tth && raw_from) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_ERROR,
			"TR and TS cannot both be given", "BBL", board, tth);
		return;
	}

	if (hasReservedRemoval(cmd)) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_ERROR, "RM has no such value",
			"BBL", board, tth);
		return;
	}

	const uint64_t permissions = boards->permissionsFor(named, user->getCredentials());

	if (!permissions) {
		/*
		 * A board this session may do nothing on answers as though it were not
		 * there, whether it is or not: withholding the descriptor is the whole of
		 * the mechanism for hiding a board, and a refusal that told a hidden board
		 * apart from an absent one would undo it.
		 *
		 * The one exception is a board an operator has marked for registered
		 * sessions, where saying so is the point - a reader who is told to register
		 * can, and one told the board does not exist cannot.
		 */
		if (boards->exists(named) && boards->registeredOnly(named))
			refuseBulletin(user, ADC_CODE_ACCESS_REGISTERED,
				"That board is for registered users", "BBL", board, tth);
		else
			refuseBulletin(user, ADC_CODE_BULLETIN_NO_BOARD, "No such board",
				"BBL", board, tth);
		return;
	}

	if (isRemoval(cmd)) {
		following.cancel(named);
		return;
	}

	if (raw_tth) {
		/*
		 * A request carrying TR is a question and not a subscription: it is answered
		 * with the one entry or with a refusal, and the session's subscriptions are
		 * not touched either way. This is how a client fills in the parent of a
		 * reply that arrived orphaned.
		 */
		if (!tth) {
			refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED,
				"Not a tiger tree hash", "BBL", board, 0, "FB", "TR");
			return;
		}

		if (!(permissions & static_cast<uint64_t>(QuickDC::Subscribe))) {
			refuseBulletin(user, ADC_CODE_ACCESS_DENIED,
				"You may not read that board", "BBL", board, tth);
			return;
		}

		const QuickDC::BoardEntry* entry = boards->findEntry(named, wanted);
		if (!entry) {
			refuseBulletin(user, ADC_CODE_BULLETIN_NO_ENTRY, "No such post",
				"BBL", board, tth);
			return;
		}

		sendOwned(writeEntry(*entry), user);
		return;
	}

	if (!(permissions & static_cast<uint64_t>(QuickDC::Subscribe))) {
		refuseBulletin(user, ADC_CODE_ACCESS_DENIED, "You may not read that board",
			"BBL", board, 0);
		return;
	}

	if (raw_from && !isNumber(raw_from)) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED, "Not a timestamp",
			"BBL", board, 0, "FB", "TS");
		return;
	}

	/* Absent means zero, which means the whole of the board. */
	const uint64_t from = raw_from ? toNumber(raw_from) : 0;

	if (!following.has(named) && following.count() >= MAX_SUBSCRIPTIONS_PER_SESSION) {
		refuseBulletin(user, ADC_CODE_BULLETIN_ERROR, "Too many subscriptions",
			"BBL", board, 0);
		return;
	}

	const std::vector<QuickDC::BoardEntry> replay = boards->since(named, from);

	/*
	 * Counted in entries, which is what makes a replay from the beginning of a long
	 * board expensive and a resumption at its head free. A client that has caught
	 * up and reconnects costs the entries of one second; a client asking for
	 * everything, over and over, costs the board each time and is refused for it.
	 *
	 * Charged at most the whole allowance, however long the board. A cost past what
	 * the bucket can ever hold would be a refusal that waiting cannot lift, so an
	 * operator who configured a board longer than the allowance would have made one
	 * that nobody can read from the beginning - and would be told to wait for it in
	 * a message that never comes true.
	 */
	const double cost = std::min(static_cast<double>(replay.size()),
		ADC::LocalHub::BOARD_REPLAY_BURST);

	if (!following.replays.spend(cost)) {
		refuseBulletin(user, ADC_CODE_BULLETIN_ERROR, "Too much of the board at once",
			"BBL", board, 0, "TL", std::to_string(following.replays.retryAfter(cost)));
		return;
	}

	/*
	 * A second BBL naming a board this session already follows replaces the first
	 * and replays from the newly requested timestamp. The cursor starts where
	 * nothing has been delivered and is moved on by each entry that goes out, so
	 * what it holds is what this session has been sent and not what it asked for.
	 */
	following.subscribe(named, 0);

	for (size_t n = 0; n < replay.size(); n++) {
		if (sendOwned(writeEntry(replay[n]), user, (n + 1) < replay.size())) {
			following.advance(named, replay[n].timestamp);
			continue;
		}

		/*
		 * The queue is at its ceiling, so the rest of the backlog has nowhere to go.
		 * The subscription is dropped rather than the replay abandoned quietly: what
		 * this session holds is a prefix of the board, and stopping there leaves it
		 * exactly that - where going on to send it live entries would leave it a
		 * prefix, a hole, and a head it had no way to know was not contiguous.
		 *
		 * It resumes from the highest timestamp it received, which is where the
		 * prefix ends, and the hole closes itself.
		 */
		QHUB("%s: '%s' could not be sent the backlog of board '%s'; dropping the "
			"subscription", ADC::SID::toString(user->sid),
			user->nick ? user->nick : "(unidentified)", named.c_str());

		following.cancel(named);
		return;
	}
}

void ADC::LocalHub::Hub::onBBP(ADC::Command* cmd) {
	CHECK_SID_OK();
	if (user->state != State_Normal) return;

	if (!hasBoards()) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FEATURE_ERROR,
			"This hub has no bulletin boards", "BBP", 0, 0);
		return;
	}

	ADC::LocalHub::Subscriptions& following = user->getSubscriptions();

	const char* raw_board = cmd->getArgument("BD", 2);
	const std::string named(raw_board ? raw_board : "");
	const bool board_ok = ADC::isValidBoardName(named);
	const char* board = board_ok ? named.c_str() : 0;

	const char* raw_tth = cmd->getArgument("TR", 2);
	const std::string hash(raw_tth ? raw_tth : "");
	const bool tth_ok = ADC::isValidTTH(hash);
	const char* tth = tth_ok ? hash.c_str() : 0;

	const char* raw_parent = cmd->getArgument("PA", 2);
	const std::string parent(raw_parent ? raw_parent : "");

	if (!raw_board) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED, "No board named",
			"BBP", 0, tth, "FM", "BD");
		return;
	}

	if (!board_ok) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED, "Not a board name",
			"BBP", 0, tth, "FB", "BD");
		return;
	}

	const uint64_t permissions = boards->permissionsFor(named, user->getCredentials());

	if (!permissions) {
		if (boards->exists(named) && boards->registeredOnly(named))
			refuseBulletin(user, ADC_CODE_ACCESS_REGISTERED,
				"That board is for registered users", "BBP", board, tth);
		else
			refuseBulletin(user, ADC_CODE_BULLETIN_NO_BOARD, "No such board",
				"BBP", board, tth);
		return;
	}

	if (!raw_tth) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED, "No post named",
			"BBP", board, 0, "FM", "TR");
		return;
	}

	if (!tth_ok) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED, "Not a tiger tree hash",
			"BBP", board, 0, "FB", "TR");
		return;
	}

	if (hasReservedRemoval(cmd)) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_ERROR, "RM has no such value",
			"BBP", board, tth);
		return;
	}

	if (isRemoval(cmd)) {
		const QuickDC::BoardEntry* entry = boards->findEntry(named, hash);

		if (!entry || entry->removed) {
			refuseBulletin(user, ADC_CODE_BULLETIN_NO_ENTRY, "No such post",
				"BBP", board, tth);
			return;
		}

		/* Copied out before the entry is withdrawn, which is what invalidates it. */
		const std::string author(entry->cid);

		const bool own = user->cid && author == user->cid;
		const bool may = (permissions & static_cast<uint64_t>(QuickDC::WithdrawAny))
			|| (own && (permissions & static_cast<uint64_t>(QuickDC::WithdrawOwn)));

		if (!may) {
			refuseBulletin(user, ADC_CODE_ACCESS_DENIED,
				"You may not withdraw that post", "BBP", board, tth);
			return;
		}

		/* A withdrawal writes to the index and rewrites its file, so it is charged
		   the same allowance a post is. */
		if (!following.posts.spend()) {
			refuseBulletin(user, ADC_CODE_BULLETIN_RATE, "Too many posts",
				"BBP", board, tth, "TL", std::to_string(following.posts.retryAfter()));
			return;
		}

		QuickDC::BoardEntry tombstone;
		if (!boards->withdraw(named, hash, tombstone)) {
			refuseBulletin(user, ADC_CODE_BULLETIN_ERROR, "The post could not be withdrawn",
				"BBP", board, tth);
			return;
		}

		QHUB("%s: '%s' withdrew %s from board '%s'", ADC::SID::toString(user->sid),
			user->nick ? user->nick : "(unidentified)", hash.c_str(), named.c_str());

		sendEntry(tombstone, user);
		announceBoard(named);
		return;
	}

	/*
	 * That the session may put anything on this board at all, which is asked here so
	 * that a board it may not post to is refused before anything else about the post
	 * is examined - a board nobody may post to answers the same way as one that is
	 * not there, and permissionsFor() returns 0 for both.
	 *
	 * Which of the two permissions is actually needed cannot be decided yet; see
	 * below, after the parent has been resolved.
	 */
	const uint64_t either = static_cast<uint64_t>(QuickDC::StartThread)
		| static_cast<uint64_t>(QuickDC::Reply);

	if (!(permissions & either)) {
		refuseBulletin(user, ADC_CODE_ACCESS_DENIED,
			"You may not post on that board", "BBP", board, tth);
		return;
	}

	const char* raw_size = cmd->getArgument("SI", 2);

	if (!raw_size) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED, "No size given",
			"BBP", board, tth, "FM", "SI");
		return;
	}

	if (!isNumber(raw_size) || !toNumber(raw_size)) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED, "Not a size",
			"BBP", board, tth, "FB", "SI");
		return;
	}

	QuickDC::Board described;
	if (!boards->describe(named, user->getCredentials(), described)) {
		refuseBulletin(user, ADC_CODE_BULLETIN_NO_BOARD, "No such board",
			"BBP", board, tth);
		return;
	}

	const uint64_t size = toNumber(raw_size);

	/*
	 * The declared size, which is all there is: nothing verifies it before the
	 * transfer, the author declares it and the hub copies it. Enforcing MS against
	 * it is what the extension asks of a hub, and a client caps the transfer
	 * itself because this number is a claim.
	 */
	if (size > described.maxSize) {
		refuseBulletin(user, ADC_CODE_BULLETIN_TOO_LARGE, "The post is too large",
			"BBP", board, tth, "MS", std::to_string(described.maxSize));
		return;
	}

	if (!parent.empty() && !ADC::isValidTTH(parent)) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED, "Not a tiger tree hash",
			"BBP", board, tth, "FB", "PA");
		return;
	}

	/*
	 * Whether this post starts a thread, decided by what will become of it rather than
	 * by what arrived. PA naming a post this board holds makes it a reply; PA naming
	 * anything else - a post on another board, one on another hub, one already
	 * withdrawn, or a hash that was never a post at all - leaves it a thread of its
	 * own, because there is no thread here to attach it to. That is what BoardStore::add()
	 * does with it, and this has to agree with that or the two answer different
	 * questions. Deciding permission from PA being present, while threading is decided
	 * from PA resolving, lets a session with Reply and not StartThread start a thread on
	 * an announcements board by quoting any hash at all as its parent - and be charged at
	 * the reply rate for doing it.
	 */
	const QuickDC::BoardEntry* attaches = parent.empty()
		? 0 : boards->findEntry(named, parent);

	const bool thread = !attaches || attaches->removed || attaches->thread.empty();

	const uint64_t needed = thread ? static_cast<uint64_t>(QuickDC::StartThread)
	                               : static_cast<uint64_t>(QuickDC::Reply);

	if (!(permissions & needed)) {
		refuseBulletin(user, ADC_CODE_ACCESS_DENIED,
			thread ? "You may not start a thread on that board"
			       : "You may not reply on that board",
			"BBP", board, tth);
		return;
	}

	if (boards->findEntry(named, hash)) {
		refuseBulletin(user, ADC_CODE_BULLETIN_ERROR, "That post is already on this board",
			"BBP", board, tth);
		return;
	}

	const char* raw_subject = cmd->getArgument("SJ", 2);
	const std::string subject(raw_subject ? raw_subject : "");

	/*
	 * The subject on an entry is a copy of a document field, offered so that a
	 * client can show a board without fetching it. Bounded because the hub keeps it
	 * and repeats it to every subscriber for as long as the board lasts, and
	 * refused rather than shortened: a truncated copy would disagree with the
	 * document under a hash that still verified, and every reader would be left to
	 * wonder which of the two the hub had altered.
	 */
	if (subject.size() > MAX_BOARD_SUBJECT
		|| subject.find('\n') != std::string::npos) {
		refuseBulletin(user, ADC_CODE_PROTOCOL_FLAG_REQUIRED, "Not a subject",
			"BBP", board, tth, "FB", "SJ");
		return;
	}

	/*
	 * Both allowances are asked before either is spent, so that a thread refused by
	 * the stricter of the two is not also charged to the other. Thread creation is
	 * held to less than replying: an existing conversation is bounded by the
	 * interest of the people in it and starting one is not.
	 */
	if (!following.posts.affords() || (thread && !following.threads.affords())) {
		const uint64_t wait = thread
			? std::max(following.posts.retryAfter(), following.threads.retryAfter())
			: following.posts.retryAfter();

		refuseBulletin(user, ADC_CODE_BULLETIN_RATE,
			thread ? "Too many new threads" : "Too many posts",
			"BBP", board, tth, "TL", std::to_string(wait));
		return;
	}

	following.posts.spend();
	if (thread) following.threads.spend();

	/*
	 * What the author declared, and what this hub attests to. ID, NI, TH and TS are
	 * never read from the command: they are not the client's to state, and building
	 * the entry from nothing rather than editing what arrived is what makes
	 * discarding them the default instead of something to remember.
	 */
	QuickDC::BoardEntry post;
	post.tth = hash;
	post.size = size;
	post.parent = parent;
	post.subject = subject;
	post.cid = user->cid ? user->cid : "";
	post.nick = user->nick ? user->nick : "";

	QuickDC::BoardEntry entry;
	const ADC::LocalHub::BoardStore::Outcome outcome = boards->add(named, post, entry);

	switch (outcome) {
		case ADC::LocalHub::BoardStore::Outcome::Accepted:
			break;

		case ADC::LocalHub::BoardStore::Outcome::NoBoard:
			refuseBulletin(user, ADC_CODE_BULLETIN_NO_BOARD, "No such board",
				"BBP", board, tth);
			return;

		case ADC::LocalHub::BoardStore::Outcome::Duplicate:
			refuseBulletin(user, ADC_CODE_BULLETIN_ERROR,
				"That post is already on this board", "BBP", board, tth);
			return;

		case ADC::LocalHub::BoardStore::Outcome::AuthorLimit:
			/*
			 * Refused rather than made room for. Dropping this author's oldest post
			 * to fit the newest would make posting a way to unpublish, and it would
			 * leave every client holding an entry the hub had quietly forgotten -
			 * where a withdrawal is a tombstone that reaches them.
			 */
			refuseBulletin(user, ADC_CODE_BULLETIN_ERROR,
				"You have as many posts on that board as you may", "BBP", board, tth);
			return;
	}

	QHUB("%s: '%s' posted %s to board '%s'", ADC::SID::toString(user->sid),
		user->nick ? user->nick : "(unidentified)", hash.c_str(), named.c_str());

	/* The entry is the acknowledgement, and there is no other. */
	sendEntry(entry, user);

	/* And the board is not what it was: its newest timestamp and its count have
	   both moved, and a session that is not subscribed learns it from here. */
	announceBoard(named);
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
		eventLoop.messages().postMessage(QuickDC::MsgHubUserDelayedQuit, 0, static_cast<size_t>(deferFor(user)), 0);
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

	/* The caller's command is the hub's now: one that no queue took ends here. */
	cmd->decRef();
	if (cmd->canDelete()) delete cmd;
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
	eventLoop.messages().postMessage(QuickDC::MsgHubUserDisconnect, 0, static_cast<size_t>(deferFor(user)), 0);
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
		eventLoop.messages().postMessage(QuickDC::MsgHubUserAppendUser, 0, static_cast<size_t>(deferFor(user)), 0);
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
		eventLoop.messages().postMessage(QuickDC::MsgHubUserRemoveUser, 0, static_cast<size_t>(deferFor(user)), 0);
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
	                || id == QuickDC::MsgHubUserDisconnect
	                || id == QuickDC::MsgConfigUpdated);

	if (!ours) return false;

	/*
	 * Put back rather than dropped.
	 *
	 * All five of these mutate the tables a broadcast walks, which is why they
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
		eventLoop.messages().postMessage(id, msg->getData(), msg->getArg1(), msg->getArg2());
		return true;
	}

	if (id == QuickDC::MsgConfigUpdated)
	{
		readConfiguration();
		if (!enabled) closeEveryConnection();
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
