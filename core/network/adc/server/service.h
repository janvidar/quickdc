/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADC_LOCALHUB_SERVICE_H
#define HAVE_QUICKDC_ADC_LOCALHUB_SERVICE_H

#include <map>
#include <string>

/* All services are prefixed with this: */
#define ADC_HUB_SERVICE_PREFIX '+'

namespace ADC {

class Command;

namespace LocalHub {

class Hub;
class ServiceMap;

class Service {
	public:
		Service(ServiceMap* map, const char* name, const char* description);
		virtual ~Service();

		Service(const Service&) = delete;
		Service& operator=(const Service&) = delete;
		virtual bool invoke(Command* cmd) = 0;
		
		const char* getName() const;
		const char* getDescription() const;
		
	protected:
		char* service;
		char* description;
		ADC::LocalHub::Hub* hub;
		ADC::LocalHub::ServiceMap* services;
	friend class ServiceMap;
};

class ServiceMap {
	public:
		ServiceMap(ADC::LocalHub::Hub* hub);
		virtual ~ServiceMap();

		ServiceMap(const ServiceMap&) = delete;
		ServiceMap& operator=(const ServiceMap&) = delete;
		
		void add(Service*);
		void remove(Service*);
		bool invoke(Command* cmd);
		
		ADC::LocalHub::Hub* getHub() const { return hub; }
		
		Service* first();
		Service* next();
		size_t size();
		
	protected:
		ADC::LocalHub::Hub* hub;
		std::map<std::string, Service*> services;
		std::map<std::string, Service*>::iterator service_it;
};


#define ADC_HUB_SERVICE_CLASS(N, NAME, DESC) \
	class N : public ADC::LocalHub::Service { \
		public: \
			N(ADC::LocalHub::ServiceMap* map) : ADC::LocalHub::Service(map, NAME, DESC) { }; \
			bool invoke(ADC::Command*) override; \
};

	ADC_HUB_SERVICE_CLASS(SrvVersion, "version", "Show server version");
	ADC_HUB_SERVICE_CLASS(SrvHelp,    "help",    "Show this help message");
	ADC_HUB_SERVICE_CLASS(SrvUptime,  "uptime",  "Show server uptime");
	ADC_HUB_SERVICE_CLASS(SrvMotd,    "motd",    "Show the message of the day");
	ADC_HUB_SERVICE_CLASS(SrvHistory, "history", "Show the most recent messages");
	ADC_HUB_SERVICE_CLASS(SrvMyIP,    "myip",    "Show your IP address");

} // namespace LocalHub
} // namespace ADC

#endif // HAVE_QUICKDC_ADC_LOCALHUB_SERVICE_H
