/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <map>
#include <vector>
#include <string.h>
#include <string>
#include <time.h>

#include "network/adc/server/adchub.h"
#include "network/adc/server/adchubuser.h"
#include "network/adc/parser.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/cid.h"
#include <samurai/io/net/socket.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/util/refcount.h>
#include <samurai/timestamp.h>

#define FOURCC(a, b, c, d) ((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d))

#define INF_STRIP_ARGUMENT(CMD, PREFIX) if (CMD->haveArgument(PREFIX, 2)) CMD->removeArgument(PREFIX, 2);
#define INF_SEND_ERROR(USER, HUB, CODE, MSG, FLAG, DATA) { \
	ADC::Command x_cmd(FOURCC('I','S','T','A')); \
	x_cmd.addArgument(CODE); \
	x_cmd.addArgument(MSG); \
	if (FLAG) x_cmd.addArgument(FLAG, DATA); \
	HUB->send(&x_cmd, USER); \
}

#define INF_CHECK_IP(CMD, SOCKET, HUB) { \
	if (CMD->haveArgument("I4", 2)) { \
		Samurai::IO::Net::InetAddress ip(CMD->getArgument("I4", 2), Samurai::IO::Net::InetAddress::IPv4); \
		Samurai::IO::Net::InetAddress any("0.0.0.0", Samurai::IO::Net::InetAddress::IPv4); \
		Samurai::IO::Net::InetAddress* actualIP = const_cast<Samurai::IO::Net::InetAddress*>(SOCKET->getAddress()); \
		if (ip == any) { \
			CMD->removeArgument("I4", 2); \
			CMD->addArgument("I4", actualIP->toString()); \
		} else if (ip != *actualIP) { \
			CMD->removeArgument("I4", 2); \
			ADC::Command x_cmd(FOURCC('I','S','T','A')); \
			x_cmd.addArgument("145"); \
			x_cmd.addArgument("Invalid IP"); \
			x_cmd.addArgument("IP", actualIP->toString()); \
			HUB->send(&x_cmd, this); \
		} \
	} \
	if (CMD->haveArgument("U4", 2)) { \
		char* str_port = CMD->getArgument("U4", 2); \
		int port = quickdc_atoi(str_port); \
		if (port <= 0 || port > 65535) { \
			CMD->removeArgument("U4", 2); \
			ADC::Command x_cmd(FOURCC('I','S','T','A')); \
			x_cmd.addArgument("143"); \
			x_cmd.addArgument("Invalid port"); \
			x_cmd.addArgument("FL", "U4"); \
			HUB->send(&x_cmd, this); \
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


ADC::HubConnection::HubConnection(Samurai::IO::Net::Socket* socket_) {
	socket = socket_;
	last_act = new Samurai::TimeStamp();
}

ADC::HubConnection::~HubConnection() {
	postMessage(Samurai::MsgSocketMonitorDelete, socket, 0, 0);
	socket->setEventHandler(0);
}

void ADC::HubConnection::resetTimer() {
	last_act->reset();
}

ADC::HubUser::HubUser(Samurai::IO::Net::Socket* socket_, ADC::Hub* hub) :
	HubConnection(socket_),
	command_sup(0),
	command_inf(0),
	nick(0),
	cid(0),
	locked(false)
{
	proxy = new CommandProxy(socket, hub);
	sid = hub->generateNextSid();
	state = ADC::State_Protocol;
	credentials = Cred_User;
	QHUB("%s: Connection from %s:%d", ADC::SID::toString(sid), socket->getAddress()->toString(), socket->getPort());
}

ADC::HubUser::~HubUser() {
	free(nick);
	free(cid);
	clearFeatures();

	state = State_None;
	delete proxy; proxy = 0;
	
	if (command_inf && command_inf->canDelete())
		delete command_inf;
	
	if (command_sup && command_sup->canDelete())
		delete command_sup;
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
		
		for (size_t n = 0; cmd->getArgument(n); n++) {
			char* arg = cmd->getArgument(n);
			
			ADC::Command* new_command_inf = new ADC::Command(command_inf);
			new_command_inf->removeArgument(arg, 2);
			new_command_inf->addArgument(arg);
			
			if (command_inf->canDelete())
				delete command_inf;
			
			command_inf = new_command_inf;
			command_inf->incRef();
		}
		
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
			if (strcmp(hu->cid, cid) || strcmp(hu->socket->getAddress()->toString(), socket->getAddress()->toString())) {
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
		
		/* Give local user operator access */
		if (strcmp(pid, ADC::CID::getInstance()->getPID()) == 0) {
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

void ADC::HubUser::addFeature(char* feature) {
	features.push_back(feature);
}

void ADC::HubUser::addFeatures(char* feats) {
	if (!feats) return;

	char* it = &feats[0];
	while (strlen(it) > 4) {
		addFeature(strndup(it, 4));
		it = &it[5];
	}
	
	if (strlen(it) > 0) {
		addFeature(strdup(it));
	}
}

void ADC::HubUser::clearFeatures() {
	while (features.size()) {
		char* feature = features.back();
		free(feature);
		features.pop_back();
	}
}

bool ADC::HubUser::hasFeature(char* feature) {
	if (!feature && strlen(feature) != 4) return false;
	for (std::vector<char*>::iterator it = features.begin(); it != features.end(); it++) {
		if (strncmp(feature, (*it), 4) == 0)
			return true;
	}
	return false;
}

size_t ADC::HubUser::getSendQueueSize() {
	return proxy->getSendQueueSize();
}

void ADC::HubUser::send(ADC::Command* cmd, bool more) {
	proxy->send(cmd);
	if (!more)
		proxy->write();
	resetTimer();
}

void ADC::HubUser::read() {
	if (proxy && state != State_WaitDisconnect) {
		locked = true;
		proxy->read();
		resetTimer();
		locked = false;
	}
}
