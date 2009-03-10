/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCHUBSERVICE_H
#define HAVE_QUICKDC_ADCHUBSERVICE_H

#include <map>
#include <string>

/* All services are prefixed with this: */
#define ADC_HUB_SERVICE_PREFIX '+'

namespace ADC {

class Command;
class Hub;
class ServiceMap;

class Service {
	public:
		Service(ServiceMap* map, const char* name, const char* description);
		virtual ~Service();
		virtual bool invoke(Command* cmd) = 0;
		
		const char* getName() const;
		const char* getDescription() const;
		
	protected:
		char* service;
		char* description;
		ADC::Hub* hub;
		ADC::ServiceMap* services;
	friend class ServiceMap;
};

class ServiceMap {
	public:
		ServiceMap(ADC::Hub* hub);
		virtual ~ServiceMap();
		
		void add(Service*);
		void remove(Service*);
		bool invoke(Command* cmd);
		
		ADC::Hub* getHub() const { return hub; }
		
		Service* first();
		Service* next();
		size_t size();
		
	protected:
		ADC::Hub* hub;
		std::map<std::string, Service*> services;
		std::map<std::string, Service*>::iterator service_it;
};


#define ADC_HUB_SERVICE_CLASS(N, NAME, DESC) \
	class N : public ADC::Service { \
		public: \
			N(ADC::ServiceMap* map) : ADC::Service(map, NAME, DESC) { }; \
			bool invoke(ADC::Command*); \
};

	ADC_HUB_SERVICE_CLASS(AdcSrvVersion, "version", "Show server version");
	ADC_HUB_SERVICE_CLASS(AdcSrvHelp,    "help",    "Show this help message");
	ADC_HUB_SERVICE_CLASS(AdcSrvUptime,  "uptime",  "Show server uptime");
	ADC_HUB_SERVICE_CLASS(AdcSrvMotd,    "motd",    "Show the message of the day");
	ADC_HUB_SERVICE_CLASS(AdcSrvHistory, "history", "Show the most recent messages");
	ADC_HUB_SERVICE_CLASS(AdcSrvMyIP,    "myip",    "Show your IP address");

}

#endif // HAVE_QUICKDC_ADCHUBSERVICE_H
