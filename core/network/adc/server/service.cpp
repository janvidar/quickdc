/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */


#include <string.h>
#include "quickdc.h"
#include "api/core.h"
#include "config/preferences.h"
#include "network/adc/server/hub.h"
#include "network/adc/server/service.h"
#include "network/adc/server/user.h"
#include "network/adc/cid.h"
#include "network/adc/parser.h"
#include <samurai/io/buffer.h>
#include <samurai/io/file.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/timestamp.h>


ADC::LocalHub::Service::Service(ADC::LocalHub::ServiceMap* map, const char* name, const char* desc) {
	service = strdup(name);
	services = map;
	description = strdup(desc);
	hub = services->getHub();
	services->add(this);
}

ADC::LocalHub::Service::~Service() {
	free(service);
	free(description);
	if (services)
		services->remove(this);
}

const char* ADC::LocalHub::Service::getName() const {
	return service;
}

const char* ADC::LocalHub::Service::getDescription() const {
	return description;
}

ADC::LocalHub::ServiceMap::ServiceMap(ADC::LocalHub::Hub* hub_) : hub(hub_) { }

ADC::LocalHub::ServiceMap::~ServiceMap() {
	while (services.size()) {
		service_it = services.begin();
		ADC::LocalHub::Service* service = (*service_it).second;
		service->services = 0;
		services.erase(service_it);
		delete service;
	}
}

void ADC::LocalHub::ServiceMap::add(Service* srv) {
	std::string s(srv->service);
	if (services.count(s)) return;
	services.insert( std::pair<std::string, Service*>(s, srv) );
}

void ADC::LocalHub::ServiceMap::remove(Service* srv) {
	std::string s(srv->service);
	if (!services.count(s)) return;
	services.erase(s);
}

ADC::LocalHub::Service* ADC::LocalHub::ServiceMap::first() {
	service_it = services.begin();
	return (*service_it).second;
}

ADC::LocalHub::Service* ADC::LocalHub::ServiceMap::next() {
	service_it++;
	if (service_it != services.end()) return (*service_it).second;
	return 0;
}

size_t ADC::LocalHub::ServiceMap::size() {
	return services.size();
}


bool ADC::LocalHub::ServiceMap::invoke(Command* cmd) {
	if (!cmd->getArgument(0)) return true;
	std::string msg(cmd->getArgument(0));
	
	if (msg.length() < 2 || msg[0] != ADC_HUB_SERVICE_PREFIX) { return true; }
	
	msg = msg.substr(1);
	size_t pos = msg.find_first_of(" ");
	if (pos != std::string::npos) {
		msg = msg.substr(0, pos);
	}
		
	if (!services.count(msg)) return true;
	return services[msg]->invoke(cmd);
}


bool ADC::LocalHub::SrvVersion::invoke(Command* cmd) {
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));

	msg->addArgument("\n"
		"*** This hub is running " PRODUCT " " VERSION " (build " BUILD ") on " SYSTEM "/" CPU "\n"
		"    Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org\n"
		);
	ADC::LocalHub::User* user  = hub->lookupUser(cmd->getSocket());
	hub->send(msg, user);
	return true;
}

bool ADC::LocalHub::SrvHelp::invoke(Command* cmd) {
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));
	
	Samurai::IO::Buffer buffer;
	buffer.append("\n*** Available commands:\n");
	
#define SERVICE_NAME_COLUMN 16
	for (ADC::LocalHub::Service* service = services->first(); service; service = services->next()) {
		buffer.append("    ");
		buffer.append(ADC_HUB_SERVICE_PREFIX);
		
		buffer.append(service->getName());
		for (size_t i = strlen(service->getName()); i < SERVICE_NAME_COLUMN; i++)
			buffer.append(' ');
	
		buffer.append("- ");
		buffer.append(service->getDescription());
		buffer.append('\n');
	}
	
	char* tbuf = new char[buffer.size()+1];
	buffer.pop(tbuf, buffer.size());
	tbuf[buffer.size()] = 0;
	
	msg->addArgument(tbuf);
	
	ADC::LocalHub::User* user  = hub->lookupUser(cmd->getSocket());
	hub->send(msg, user);
	
	delete[] tbuf;
	return true;
}

bool ADC::LocalHub::SrvUptime::invoke(Command* cmd) {
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));
	Samurai::TimeStamp startup = hub->startedAt();
	uint32_t seconds = startup.elapsed();
	uint32_t days  = static_cast<int>(seconds) / 86400;
	uint32_t temp  = (seconds - (days * 86400));
	uint32_t hours = static_cast<int>(temp) / 3600;
	uint32_t minutes = static_cast<int>(temp % 3600) / 60;
	char buf[100] = { 0, };
	if (startup.elapsed() < (86400 * 2)) {
		if (hours > 2) {
			snprintf(buf, 100, "*** Uptime: %u hours.", hours);
		} else {
			uint32_t n_minutes = (hours * 60) + minutes;
			snprintf(buf, 100, "*** Uptime: %u %s.", n_minutes, n_minutes == 1 ? "minute" : "minutes");
		}
	} else {
		snprintf(buf, 100, "*** Uptime: %u days, %u %s.", days, hours, hours == 1 ? "hour" : "hours");
	}
	msg->addArgument(buf);
	ADC::LocalHub::User* user  = hub->lookupUser(cmd->getSocket());
	hub->send(msg, user);
	return true;
}

#define MAX_MOTD_LEN 64 * 1024

/* Read on every request rather than cached: two hubs in one process have two files. */
bool ADC::LocalHub::SrvMotd::invoke(Command* cmd) {
	ADC::LocalHub::User* user  = hub->lookupUser(cmd->getSocket());
	if (!user) return false;

	Samurai::IO::File motd(hub->directory() + "/motd");
	if (!motd.exists() || !motd.isReadable() || !motd.open(Samurai::IO::File::Mode::Read)) {
		QDBG("Unable to open motd message file: %s", motd.getName().c_str());
		return false;
	}

	const size_t size = motd.size();
	if (size > MAX_MOTD_LEN) {
		QDBG("Motd message is too large to send (!)");
		return false;
	}

	std::string text(size, '\0');
	const ssize_t ret = motd.read(text.data(), size);
	if (ret <= 0) return false;
	text.resize(static_cast<size_t>(ret));

	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));
	msg->addArgument(text.c_str());
	hub->send(msg, user);
	return true;
}


bool ADC::LocalHub::SrvHistory::invoke(Command* cmd) {
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));
	
	Samurai::IO::Buffer buffer;
	if (hub->history.size() == 0) {
		buffer.append("*** No messages");
	} else {
		buffer.append("*** Showing the ");
		buffer.append(static_cast<int>(hub->history.size()));
		buffer.append(" most recent message"); 
		if (hub->history.size() > 1) buffer.append('s');
		buffer.append(":\n");
	
		for (std::deque<char*>::iterator it = hub->history.begin(); it != hub->history.end(); it++) {
			buffer.append(*it);
			buffer.append('\n');
		}
	}
	
	char* tbuf = new char[buffer.size()+1];
	buffer.pop(tbuf, buffer.size());
	tbuf[buffer.size()] = 0;
	msg->addArgument(tbuf);
	ADC::LocalHub::User* user  = hub->lookupUser(cmd->getSocket());
	hub->send(msg, user);
	delete[] tbuf;
	return true;
}

bool ADC::LocalHub::SrvMyIP::invoke(Command* cmd) {
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));

	/*
	 * getAddress() answers null for a socket whose peer has gone, and a service
	 * can be invoked from a command that carries no socket at all, so neither is
	 * dereferenced without asking. Saying so is the honest answer; there is no
	 * address to substitute.
	 */
	const Samurai::IO::Net::Socket* socket = cmd->getSocket();
	const Samurai::IO::Net::InetAddress* address = socket ? socket->getAddress() : 0;
	const std::string text = address
		? "*** Your IP address is: " + address->toString() + "."
		: std::string("*** Your IP address is not known.");
	msg->addArgument(text.c_str());
	ADC::LocalHub::User* user  = hub->lookupUser(cmd->getSocket());
	hub->send(msg, user);
	return true;
}

