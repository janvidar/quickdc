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

#include "network/adc/server/adchub.h"
#include "network/adc/server/adchubuser.h"
#include "network/adc/parser.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/cid.h"
#include <samurai/io/net/socket.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/timestamp.h>

#define FOURCC(a, b, c, d) ((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d))

#define INF_STRIP_ARGUMENT(CMD, PREFIX) if (CMD->haveArgument(PREFIX, 2)) CMD->removeArgument(PREFIX, 2);
/*
 * NOTE: these commands are heap allocated, where they used to be locals. Hub::send()
 * hands the command to the user's CommandFIFO, which keeps it queued whenever the
 * socket only took part of it - so the queue outlived the block a local was declared
 * in, and went on to read, and finally delete, a dead stack frame.
 */
#define INF_SEND_ERROR(USER, HUB, CODE, MSG, FLAG, DATA) { \
	ADC::Command* x_cmd = new ADC::Command(FOURCC('I','S','T','A')); \
	x_cmd->addArgument(CODE); \
	x_cmd->addArgument(MSG); \
	if (FLAG) x_cmd->addArgument(FLAG, DATA); \
	HUB->send(x_cmd, USER); \
}

#define INF_CHECK_IP(CMD, SOCKET, HUB) { \
	if (CMD->haveArgument("I4", 2)) { \
		Samurai::IO::Net::InetAddress ip(CMD->getArgument("I4", 2), Samurai::IO::Net::InetAddress::Version::IPv4); \
		Samurai::IO::Net::InetAddress any("0.0.0.0", Samurai::IO::Net::InetAddress::Version::IPv4); \
		Samurai::IO::Net::InetAddress* actualIP = const_cast<Samurai::IO::Net::InetAddress*>(SOCKET->getAddress()); \
		if (ip == any) { \
			CMD->removeArgument("I4", 2); \
			CMD->addArgument("I4", actualIP->toString()); \
		} else if (ip != *actualIP) { \
			CMD->removeArgument("I4", 2); \
			ADC::Command* x_cmd = new ADC::Command(FOURCC('I','S','T','A')); \
			x_cmd->addArgument("145"); \
			x_cmd->addArgument("Invalid IP"); \
			x_cmd->addArgument("IP", actualIP->toString()); \
			HUB->send(x_cmd, this); \
		} \
	} \
	if (CMD->haveArgument("U4", 2)) { \
		char* str_port = CMD->getArgument("U4", 2); \
		int port = Samurai::Util::Convert::to_int32(str_port); \
		if (port <= 0 || port > 65535) { \
			CMD->removeArgument("U4", 2); \
			ADC::Command* x_cmd = new ADC::Command(FOURCC('I','S','T','A')); \
			x_cmd->addArgument("143"); \
			x_cmd->addArgument("Invalid port"); \
			x_cmd->addArgument("FL", "U4"); \
			HUB->send(x_cmd, this); \
		} \
	} \
}

#define INF_EXTRACT_FEATURE(CMD) { \
	if (cmd->haveArgument("SU", 2)) { \
		char* ft_str = cmd->getArgument("SU", 2); \
		clearFeatures(); \
		if (ft_str && strlen(ft_str) > 0) addFeatures(ft_str); \
	} \
}


ADC::HubConnection::HubConnection(std::shared_ptr<Samurai::IO::Net::Socket> socket_) {
	socket = std::move(socket_);
}

ADC::HubConnection::~HubConnection()
{
	/*
	 * A user is normally dropped from inside one of its socket's own event
	 * handlers. The monitor holds the socket for the duration of that callback,
	 * so letting go of the last reference here is safe.
	 */
	socket->setEventHandler(0);
	socket.reset();
}

void ADC::HubConnection::resetTimer() {
	last_act.reset();
}

time_t ADC::HubConnection::getIdleTime() {
	return last_act.elapsed();
}

ADC::HubUser::HubUser(std::shared_ptr<Samurai::IO::Net::Socket> socket_, ADC::Hub* hub) :
	HubConnection(std::move(socket_)),
	command_sup(0),
	command_inf(0),
	nick(0),
	cid(0),
	locked(false),
	tokens(ADC::HUB_COMMAND_BURST),
	tokens_filled(std::chrono::steady_clock::now()),
	disconnecting(false),
	quitting(false),
	announce_quit(false)
{
	proxy = new CommandProxy(socket, hub);
	proxy->setSendQueueLimit(hub->hardSendQ);
	sid = hub->generateNextSid();
	state = ADC::State_Protocol;
	credentials = Cred_User;
	QHUB("%s: Connection from %s:%d", ADC::SID::toString(sid), socket->getAddress()->toString().c_str(), socket->getPort());
}

ADC::HubUser::~HubUser() {
	free(nick);
	free(cid);
	clearFeatures();

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

bool ADC::HubUser::setINF(ADC::Command* cmd, ADC::Hub* hub) {
	
	/* These parameters can only be set by the hub, and must
	   therefore be removed */
	INF_STRIP_ARGUMENT(cmd, "OP");
	INF_STRIP_ARGUMENT(cmd, "HI");
	INF_STRIP_ARGUMENT(cmd, "HU");
	INF_STRIP_ARGUMENT(cmd, "BO");
	INF_STRIP_ARGUMENT(cmd, "TO");
	INF_STRIP_ARGUMENT(cmd, "I6"); // FIXME: we don't support this - and cannot verify it!
	INF_STRIP_ARGUMENT(cmd, "U6"); // FIXME: we don't support this - and cannot verify it!
	
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
		
		if (!strlen(nick) || !strcasecmp(nick, hub->hubname)) { /* FIXME: NICK NOT VALID */
			INF_SEND_ERROR(this, hub, "221", "Invalid nick", 0, 0);
			return false;
		}
	
		if (HubUser* hu = hub->lookupUser(nick)) {
			if (strcmp(hu->cid, cid) || hu->socket->getAddress()->toString() != socket->getAddress()->toString()) {
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
		
		/*
		 * Give the local user operator access.
		 *
		 * Recognised by the address it connects from, not by its PID as this
		 * used to be. ADC requires a client to send its PID to every hub it
		 * joins - it is how the hub checks the CID is the PID's hash - so every
		 * hub the local client has ever visited holds the value that used to be
		 * enough to become admin here. The comparison was not constant time
		 * either, and the outcome is echoed straight back in the OP flag.
		 */
		if (socket->getAddress()->isLoopback()) {
			credentials = Cred_Admin;
			cmd->addArgument("OP", "1");
		}

		cmd->removeArgument("PD", 2);
		
		command_inf = new ADC::Command(cmd);
		command_inf->incRef();
		return true;

	} else {
	
	}
	return false;
}

void ADC::HubUser::addFeature(const std::string& feature) {
	if (features.size() >= ADC::MAX_USER_FEATURES) return;
	features.insert(feature);
}

/*
 * The field is a run of four character names, each followed by a separator, and
 * the user decides how long it is. Walked as a view rather than by asking for the
 * remaining length each time around, which was itself quadratic in that length,
 * and stopped once the ceiling is reached.
 */
void ADC::HubUser::addFeatures(const char* feats) {
	if (!feats) return;

	std::string_view rest(feats);
	while (!rest.empty() && features.size() < ADC::MAX_USER_FEATURES) {
		addFeature(std::string(rest.substr(0, std::min<size_t>(4, rest.size()))));
		rest.remove_prefix(std::min<size_t>(5, rest.size()));
	}
}

void ADC::HubUser::clearFeatures() {
	features.clear();
}

bool ADC::HubUser::hasFeature(const char* feature) const {
	/* Either of these disqualifies it; the test used to be an 'and', which meant
	   a null feature reached strlen(). */
	if (!feature || strlen(feature) != 4) return false;
	return features.count(feature) != 0;
}

bool ADC::HubUser::allowCommand() {
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	const std::chrono::duration<double> elapsed = now - tokens_filled;
	tokens_filled = now;

	tokens += elapsed.count() * ADC::HUB_COMMAND_RATE;
	if (tokens > ADC::HUB_COMMAND_BURST)
		tokens = ADC::HUB_COMMAND_BURST;

	if (tokens < 1.0) return false;

	tokens -= 1.0;
	return true;
}

size_t ADC::HubUser::getSendQueueSize() {
	return proxy->getSendQueueSize();
}

bool ADC::HubUser::send(ADC::Command* cmd, bool more) {
	const bool queued = proxy->send(cmd);
	if (!more)
		proxy->write();
	resetTimer();
	return queued;
}

void ADC::HubUser::read() {
	if (proxy && state != State_WaitDisconnect) {
		locked = true;
		proxy->read();
		resetTimer();
		locked = false;
	}
}
