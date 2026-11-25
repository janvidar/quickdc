/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <vector>
#include <string.h>
#include <string>
#include <string_view>
#include <time.h>

#include "network/adc/server/acl.h"
#include "network/adc/server/boards.h"
#include "network/adc/server/hub.h"
#include "network/adc/server/user.h"
#include "network/adc/parser.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/cid.h"
#include <samurai/io/net/socket.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/timestamp.h>

#define INF_STRIP_ARGUMENT(CMD, PREFIX) if (CMD->haveArgument(PREFIX, 2)) CMD->removeArgument(PREFIX, 2);
/*
 * NOTE: these commands are heap allocated, where they used to be locals. Hub::send()
 * hands the command to the user's CommandFIFO, which keeps it queued whenever the
 * socket only took part of it - so the queue outlived the block a local was declared
 * in, and went on to read, and finally delete, a dead stack frame.
 */
/*
 * Where a socket's peer is, or nothing.
 *
 * getAddress() answers null for a socket that has no peer - one that never
 * connected, or whose peer has already gone - and a login can be processed after
 * that has happened, so the three places below that used to dereference it
 * without asking would crash on a peer that hung up at the wrong moment.
 *
 * Nothing rather than a placeholder, so that two unknown addresses do not
 * compare equal: an address that is not known is not the same address.
 */
static std::string peerAddress(const Samurai::IO::Net::Socket* socket)
{
	if (!socket) return std::string();
	const Samurai::IO::Net::InetAddress* addr = socket->getAddress();
	return addr ? addr->toString() : std::string();
}

#define INF_SEND_ERROR(USER, HUB, CODE, MSG, FLAG, DATA) { \
	ADC::Command* x_cmd = new ADC::Command(FOURCC('I','S','T','A')); \
	x_cmd->addArgument(CODE); \
	x_cmd->addArgument(MSG); \
	if (FLAG) x_cmd->addArgument(FLAG, DATA); \
	HUB->send(x_cmd, USER); \
}

/*
 * Hold an announced endpoint to what the connection itself shows.
 *
 * A client's address is the one thing in an INF the hub can check rather than
 * take on trust, and it checks it by comparing what was claimed against where
 * the connection came from. That comparison only means anything on the family
 * the connection actually uses: a peer reaching this hub over IPv4 has no IPv6
 * address here to be checked against, so a claim about the other family is
 * stripped rather than believed or argued with - unverifiable is not the same as
 * wrong, and answering it with "Invalid IP" would quote an address of the wrong
 * family back at them.
 *
 * The wildcard is a request rather than a claim: a client that does not know
 * where it is says so, and the hub fills in what it sees.
 */
#define INF_CHECK_ADDRESS(CMD, SOCKET, HUB, FIELD, PORTFIELD, VERSION, ANY) { \
	const Samurai::IO::Net::InetAddress* actualIP = SOCKET->getAddress(); \
	const bool sameFamily = actualIP && actualIP->getType() == VERSION; \
	if (CMD->haveArgument(FIELD, 2)) { \
		Samurai::IO::Net::InetAddress ip(CMD->getArgument(FIELD, 2), VERSION); \
		Samurai::IO::Net::InetAddress any(ANY, VERSION); \
		if (!sameFamily) { \
			/* No address of this family to compare against, nor to substitute. */ \
			CMD->removeArgument(FIELD, 2); \
		} else if (ip == any) { \
			CMD->removeArgument(FIELD, 2); \
			CMD->addArgument(FIELD, actualIP->toString()); \
		} else if (ip != *actualIP) { \
			CMD->removeArgument(FIELD, 2); \
			ADC::Command* x_cmd = new ADC::Command(FOURCC('I','S','T','A')); \
			/* Severity 1 and ADC_CODE_PROTOCOL_IP_INF_ERROR, which is the code \
			   that carries 'IP'. This said 145, which is the feature error and \
			   says to specify 'FC' - and a client reading it as one has been \
			   told something unrecoverable rather than told its own address. */ \
			x_cmd->addArgument("146"); \
			x_cmd->addArgument("Invalid IP"); \
			x_cmd->addArgument("IP", actualIP->toString()); \
			HUB->send(x_cmd, this); \
		} \
	} \
	if (CMD->haveArgument(PORTFIELD, 2)) { \
		char* str_port = CMD->getArgument(PORTFIELD, 2); \
		int port = Samurai::Util::Convert::to_int32(str_port); \
		if (!sameFamily || port <= 0 || port > 65535) { \
			CMD->removeArgument(PORTFIELD, 2); \
			if (sameFamily) { \
				ADC::Command* x_cmd = new ADC::Command(FOURCC('I','S','T','A')); \
				x_cmd->addArgument("143"); \
				x_cmd->addArgument("Invalid port"); \
				x_cmd->addArgument("FL", PORTFIELD); \
				HUB->send(x_cmd, this); \
			} \
		} \
	} \
}

#define INF_CHECK_IP(CMD, SOCKET, HUB) { \
	INF_CHECK_ADDRESS(CMD, SOCKET, HUB, "I4", "U4", \
	                  Samurai::IO::Net::InetAddress::Version::IPv4, "0.0.0.0"); \
	INF_CHECK_ADDRESS(CMD, SOCKET, HUB, "I6", "U6", \
	                  Samurai::IO::Net::InetAddress::Version::IPv6, "::"); \
}

#define INF_EXTRACT_FEATURE(CMD) { \
	if (cmd->haveArgument("SU", 2)) { \
		char* ft_str = cmd->getArgument("SU", 2); \
		clearFeatures(); \
		if (ft_str && strlen(ft_str) > 0) addFeatures(ft_str); \
	} \
}


ADC::LocalHub::Connection::Connection(std::shared_ptr<Samurai::IO::Net::Socket> socket_) {
	socket = std::move(socket_);
}

ADC::LocalHub::Connection::~Connection()
{
	/*
	 * A user is normally dropped from inside one of its socket's own event
	 * handlers. The monitor holds the socket for the duration of that callback,
	 * so letting go of the last reference here is safe.
	 */
	socket->setEventHandler(0);
	socket.reset();
}

void ADC::LocalHub::Connection::resetTimer() {
	last_act.reset();
}

time_t ADC::LocalHub::Connection::getIdleTime() {
	return last_act.elapsed();
}

ADC::LocalHub::User::User(std::shared_ptr<Samurai::IO::Net::Socket> socket_, ADC::LocalHub::Hub* hub) :
	Connection(std::move(socket_)),
	command_sup(0),
	command_inf(0),
	nick(0),
	cid(0),
	subscriptions(new Subscriptions()),
	locked(false),
	tokens(ADC::LocalHub::HUB_COMMAND_BURST),
	tokens_filled(std::chrono::steady_clock::now()),
	disconnecting(false),
	quitting(false),
	announce_quit(false)
{
	proxy = new CommandProxy(socket, hub);
	proxy->setSendQueueLimit(hub->hardSendQ);
	sid = hub->generateNextSid();
	state = ADC::LocalHub::State_Protocol;
	credentials = Cred_User;
	/* Empty is what peerAddress() answers for a socket with no peer, which is
	   deliberate for the comparison below - said plainly here instead. */
	const std::string from = peerAddress(socket.get());
	QHUB("%s: Connection from %s:%d", ADC::SID::toString(sid),
		from.empty() ? "(unknown)" : from.c_str(), socket->getPort());
}

ADC::LocalHub::User::~User() {
	free(nick);
	free(cid);
	clearFeatures();

	/* And with it every subscription this session held, which is what makes a
	   connection ending the end of them. */
	delete subscriptions; subscriptions = 0;

	state = State_None;
	delete proxy; proxy = 0;

	/*
	 * The reference this user took has to be given back before asking whether
	 * anyone is left holding the command: while the user held one, canDelete()
	 * could never be true, so the INF of every user that ever left was leaked.
	 * A copy still sitting in someone else's send queue keeps its own reference
	 * and is freed when that queue lets go of it.
	 */
	if (command_inf) {
		command_inf->decRef();
		if (command_inf->canDelete())
			delete command_inf;
	}

	if (command_sup) {
		command_sup->decRef();
		if (command_sup->canDelete())
			delete command_sup;
	}
}

bool ADC::LocalHub::User::setINF(ADC::Command* cmd, ADC::LocalHub::Hub* hub) {
	
	/* These parameters can only be set by the hub, and must
	   therefore be removed */
	INF_STRIP_ARGUMENT(cmd, "CT");
	INF_STRIP_ARGUMENT(cmd, "HI");
	INF_STRIP_ARGUMENT(cmd, "HU");
	INF_STRIP_ARGUMENT(cmd, "BO");
	INF_STRIP_ARGUMENT(cmd, "TO");
	
	if (state == State_Normal && command_inf) {
	
		/* These cannot be changed at this stage! */
		INF_STRIP_ARGUMENT(cmd, "ID");
		INF_STRIP_ARGUMENT(cmd, "PD");
		INF_STRIP_ARGUMENT(cmd, "NI");
		INF_CHECK_IP(cmd, socket, hub);
		
		/*
		 * One copy for the whole update, not one per argument.
		 *
		 * A copy is still needed: the accumulated INF is what the hub hands to
		 * every other user, so a copy of it may be sitting in someone's send
		 * queue with its raw form already partly written out - and mutating an
		 * argument resets that cache underneath them. The reference this user
		 * holds moves to the new copy, and the old one is freed here unless a
		 * queue still holds it.
		 */
		ADC::Command* new_command_inf = new ADC::Command(command_inf);

		for (size_t n = 0; cmd->getArgument(n); n++) {
			char* arg = cmd->getArgument(n);
			new_command_inf->removeArgument(arg, 2);
			new_command_inf->addArgument(arg);
		}

		command_inf->decRef();
		if (command_inf->canDelete())
			delete command_inf;

		command_inf = new_command_inf;
		command_inf->incRef();


		INF_EXTRACT_FEATURE(cmd);

		return cmd->countArguments();
	
	} else if (state == State_Identify) {
		
		// MUST CONTAIN: ID, PD, NI
		if (!cmd->haveArgument("ID", 2)) {
			INF_SEND_ERROR(this, hub, "243", "Missing CID", "FL", "ID");
			return false;
		}

		if (!cmd->haveArgument("PD", 2)) {
			INF_SEND_ERROR(this, hub, "243", "Missing PID", "FL", "PD");
			return false;
		}

		if (!cmd->haveArgument("NI", 2)) {
			INF_SEND_ERROR(this, hub, "243", "Missing nick", "FL", "NI");
			return false;
		}
		
		char* pid  = cmd->getArgument("PD", 2);
		cid  = strdup(cmd->getArgument("ID", 2));
		nick = strdup(cmd->getArgument("NI", 2));

		INF_EXTRACT_FEATURE(cmd);
		
		if (!ADC::CID::verifyCIDandPID(cid, pid)) {
			INF_SEND_ERROR(this, hub, "227", "Invalid CID/PID", 0, 0);
			return false;
		}
		
		const std::string_view claimed(nick);

		if (claimed.empty() || Samurai::Util::iequals(claimed, hub->hubname ? hub->hubname : "")) { /* FIXME: NICK NOT VALID */
			INF_SEND_ERROR(this, hub, "221", "Invalid nick", 0, 0);
			return false;
		}

		if (const ACL::Authorization* banned = hub->acl->banFor(cid, nick, peerAddress(socket.get()))) {
			const char* reason = banned->getDescription() ? banned->getDescription() : "Banned";
			const time_t until = banned->getExpireTime().getInternalData();
			if (until == 0) {
				INF_SEND_ERROR(this, hub, "231", reason, 0, 0);
			} else {
				const std::string left = std::to_string(std::max<time_t>(until - time(0), 1));
				INF_SEND_ERROR(this, hub, "232", reason, "TL", left.c_str());
			}
			return false;
		}
	
		if (User* hu = hub->lookupUser(nick)) {
			/*
			 * Fail closed on an address that is not known: an unknown one is not
			 * a match, so the old session is left alone and the new one refused
			 * rather than the old one being kicked on a guess.
			 */
			const std::string existing = peerAddress(hu->socket.get());
			const std::string incoming = peerAddress(socket.get());
			const bool sameAddress = !existing.empty() && existing == incoming;

			if (std::string_view(hu->cid) != cid || !sameAddress) {
				INF_SEND_ERROR(this, hub, "222", "User already logged in.", 0, 0);
				return false;
			} else {
				QHUB("%s: Disconnecting old user with same IP/nick/CID.", ADC::SID::toString(sid));
				hub->kick(hu);
			
				ADC::Command* quit = new ADC::Command(FOURCC('I','Q','U','I'));
				quit->addArgument(ADC::SID::toString(hu->sid));
				// quit->addArgument("MS", "Connection dropped");
				hub->send(quit);
			}
		}
		
		if (hub->lookupUserCID(cid)) {
			INF_SEND_ERROR(this, hub, "224", "User already logged in.", 0, 0);
			return false;
		}
		
		INF_CHECK_IP(cmd, socket, hub);
		
#ifdef QUICKDC_ALLOW_LOOPBACK_AUTH
		/*
		 * Give a client connecting over the loopback administrator access.
		 *
		 * Behind the same option that governs authorising a peer over the
		 * loopback, because it is the same trust and the same threat: any local
		 * account, and anything reached through a forward to the hub's port,
		 * becomes an administrator of this hub - able to kick, ban and redirect
		 * every other user on it. It used to be unconditional.
		 *
		 * Recognised by the address it connects from, not by its PID as this once
		 * was. ADC requires a client to send its PID to every hub it joins - it is
		 * how the hub checks the CID is the PID's hash - so every hub the local
		 * client has ever visited held the value that used to be enough to become
		 * administrator here. That comparison was not constant time either, and
		 * its outcome was echoed straight back in the type flag.
		 */
		const Samurai::IO::Net::InetAddress* from = socket->getAddress();
		if (from && from->isLoopback()) {
			QERR("SECURITY: making a loopback client an administrator of this hub, "
				"because this build has QUICKDC_ALLOW_LOOPBACK_AUTH");
			credentials = Cred_Admin;

			/* Owner, super user and operator together: the spec has each of the
			   upper two imply the ones below it, and says to send them so. */
			cmd->addArgument("CT", "28");
		}
#endif

		cmd->removeArgument("PD", 2);
		
		command_inf = new ADC::Command(cmd);
		command_inf->incRef();
		return true;

	} else {
	
	}
	return false;
}

void ADC::LocalHub::User::addFeature(const std::string& feature) {
	if (features.size() >= ADC::LocalHub::MAX_USER_FEATURES) return;
	features.insert(feature);
}

/*
 * The field is a run of four character names, each followed by a separator, and
 * the user decides how long it is. Walked as a view rather than by asking for the
 * remaining length each time around, which was itself quadratic in that length,
 * and stopped once the ceiling is reached.
 */
void ADC::LocalHub::User::addFeatures(const char* feats) {
	if (!feats) return;

	std::string_view rest(feats);
	while (!rest.empty() && features.size() < ADC::LocalHub::MAX_USER_FEATURES) {
		addFeature(std::string(rest.substr(0, std::min<size_t>(4, rest.size()))));
		rest.remove_prefix(std::min<size_t>(5, rest.size()));
	}
}

void ADC::LocalHub::User::clearFeatures() {
	features.clear();
}

void ADC::LocalHub::User::addExtension(const std::string& extension) {
	if (extensions.size() >= ADC::LocalHub::MAX_USER_EXTENSIONS) return;
	extensions.insert(extension);
}

/*
 * A client may take back an extension it offered, by naming it with 'RM' in a
 * later SUP. Withdrawing one it never offered is not an error: the client has
 * said it does not want the thing, and that is the state it is left in.
 */
void ADC::LocalHub::User::removeExtension(const std::string& extension) {
	extensions.erase(extension);
}

bool ADC::LocalHub::User::supportsExtension(const char* extension) const {
	/* As hasFeature(): a name that is not four characters is not one a SUP can
	   have carried, and asking must not read through a null. */
	if (!extension || strlen(extension) != 4) return false;
	return extensions.count(extension) != 0;
}

std::string ADC::LocalHub::User::getObservedAddress() const
{
	return peerAddress(socket.get());
}


bool ADC::LocalHub::User::hasFeature(const char* feature) const {
	/* Either of these disqualifies it; the test used to be an 'and', which meant
	   a null feature reached strlen(). */
	if (!feature || strlen(feature) != 4) return false;
	return features.count(feature) != 0;
}

bool ADC::LocalHub::User::allowCommand() {
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	const std::chrono::duration<double> elapsed = now - tokens_filled;
	tokens_filled = now;

	tokens += elapsed.count() * ADC::LocalHub::HUB_COMMAND_RATE;
	if (tokens > ADC::LocalHub::HUB_COMMAND_BURST)
		tokens = ADC::LocalHub::HUB_COMMAND_BURST;

	if (tokens < 1.0) return false;

	tokens -= 1.0;
	return true;
}

size_t ADC::LocalHub::User::getSendQueueSize() {
	return proxy->getSendQueueSize();
}

bool ADC::LocalHub::User::send(ADC::Command* cmd, bool more) {
	const bool queued = proxy->send(cmd);
	if (!more)
		proxy->write();
	resetTimer();
	return queued;
}

void ADC::LocalHub::User::read() {
	if (!proxy) return;

	/*
	 * Whatever a user awaiting disconnection sends is thrown away rather than
	 * left on the socket. Not acting on it is the whole point of the state - the
	 * hub has finished with this user and is only waiting for the kick to go out -
	 * but the socket monitor is level triggered, so a socket nobody reads is
	 * reported readable on every pass of the event loop and the hub spins at full
	 * speed for as long as the user sits there. Returning here was that spin.
	 *
	 * Deliberately without resetTimer(): a peer that goes on sending must not be
	 * able to hold itself in this state, which is what the sweep's linger deadline
	 * measures - see Hub::EventTimeout().
	 */
	if (state == State_WaitDisconnect) {
		proxy->discard();
		return;
	}

	locked = true;
	proxy->read();
	resetTimer();
	locked = false;
}
