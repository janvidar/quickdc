/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */


#include <string.h>
#include "quickdc.h"
#include "api/core.h"
#include "config/preferences.h"
#include "network/adc/server/adchub.h"
#include "network/adc/server/adchubservice.h"
#include "network/adc/server/adchubuser.h"
#include "network/adc/cid.h"
#include "network/adc/parser.h"
#include <samurai/io/buffer.h>
#include <samurai/io/file.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/timestamp.h>

#define LOCALHUB QuickDC::Core::getInstance()->localHub

ADC::Service::Service(ADC::ServiceMap* map, const char* name, const char* desc) {
	service = strdup(name);
	services = map;
	description = strdup(desc);
	hub = services->getHub();
	services->add(this);
}

ADC::Service::~Service() {
	free(service);
	free(description);
	if (services)
		services->remove(this);
}

const char* ADC::Service::getName() const {
	return service;
}

const char* ADC::Service::getDescription() const {
	return description;
}

ADC::ServiceMap::ServiceMap(ADC::Hub* hub_) : hub(hub_) { }

ADC::ServiceMap::~ServiceMap() {
	while (services.size()) {
		service_it = services.begin();
		ADC::Service* service = (*service_it).second;
		service->services = 0;
		services.erase(service_it);
		delete service;
	}
}

void ADC::ServiceMap::add(Service* srv) {
	std::string s(srv->service);
	if (services.count(s)) return;
	services.insert( std::pair<std::string, Service*>(s, srv) );
}

void ADC::ServiceMap::remove(Service* srv) {
	std::string s(srv->service);
	if (!services.count(s)) return;
	services.erase(s);
}

ADC::Service* ADC::ServiceMap::first() {
	service_it = services.begin();
	return (*service_it).second;
}

ADC::Service* ADC::ServiceMap::next() {
	service_it++;
	if (service_it != services.end()) return (*service_it).second;
	return 0;
}

size_t ADC::ServiceMap::size() {
	return services.size();
}


bool ADC::ServiceMap::invoke(Command* cmd) {
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


bool ADC::AdcSrvVersion::invoke(Command* cmd) {
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));

	msg->addArgument("\n"
		"*** This hub is running " PRODUCT " " VERSION " (build " BUILD ") on " SYSTEM "/" CPU "\n"
		"    Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org\n"
		);
	ADC::HubUser* user  = hub->lookupUser(cmd->getSocket());
	hub->send(msg, user);
	return true;
}

bool ADC::AdcSrvHelp::invoke(Command* cmd) {
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));
	
	Samurai::IO::Buffer buffer;
	buffer.append("\n*** Available commands:\n");
	
#define SERVICE_NAME_COLUMN 16
	for (ADC::Service* service = services->first(); service; service = services->next()) {
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
	
	ADC::HubUser* user  = hub->lookupUser(cmd->getSocket());
	hub->send(msg, user);
	
	delete[] tbuf;
	return true;
}

bool ADC::AdcSrvUptime::invoke(Command* cmd) {
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));
	Samurai::TimeStamp startup = QuickDC::Core::getInstance()->startup;
	uint32_t seconds = startup.elapsed();
	uint32_t days  = (int) seconds / 86400;
	uint32_t temp  = (seconds - (days * 86400));
	uint32_t hours = (int) temp / 3600;
	uint32_t minutes = (int) (temp % 3600) / 60;
	static char buf[100] = { 0, };
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
	ADC::HubUser* user  = hub->lookupUser(cmd->getSocket());
	hub->send(msg, user);
	return true;
}

#define MAX_MOTD_LEN 64 * 1024
bool ADC::AdcSrvMotd::invoke(Command* cmd) {
	static Samurai::TimeStamp last_mod_time(0);
	static char motd_buffer[MAX_MOTD_LEN] = {0, };

	ADC::HubUser* user  = hub->lookupUser(cmd->getSocket());
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));

	bool sendMessage = true;
	Samurai::IO::File motd("~/.quickdc/hub/motd");
	if (motd.getTimeModified() > last_mod_time) {
	
		if (user && motd.exists() && motd.isReadable() && motd.open(Samurai::IO::File::Read)) {
			size_t size = motd.size();
			if (size >  MAX_MOTD_LEN) {
				QDBG("Motd message is too large to be cached (!)");
				sendMessage = false;
			} else {
				memset(motd_buffer, 0, MAX_MOTD_LEN);
				ssize_t ret = motd.read(motd_buffer, size);
				if (ret > 0)
					last_mod_time = motd.getTimeModified();
			}
		} else {
			QDBG("Unable to open motd message file: %s", motd.getName().c_str());
			sendMessage = false;
		}
	}

	if (sendMessage) {
		msg->addArgument(motd_buffer);
		// msg->addArgument("PM", hubsid);
		hub->send(msg, user);
		return true;
	}

	return false;
}


bool ADC::AdcSrvHistory::invoke(Command* cmd) {
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));
	
	Samurai::IO::Buffer buffer;
	if (hub->history.size() == 0) {
		buffer.append("*** No messages");
	} else {
		buffer.append("*** Showing the ");
		buffer.append((int) hub->history.size());
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
	ADC::HubUser* user  = hub->lookupUser(cmd->getSocket());
	hub->send(msg, user);
	delete[] tbuf;
	return true;
}

bool ADC::AdcSrvMyIP::invoke(Command* cmd) {
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));
	static char buf[100];
	snprintf(buf, 100, "*** Your IP address is: %s.", cmd->getSocket()->getAddress()->toString());
	msg->addArgument(buf);
	ADC::HubUser* user  = hub->lookupUser(cmd->getSocket());
	hub->send(msg, user);
	return true;
}

