/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCHUBUSER_H
#define HAVE_QUICKDC_ADCHUBUSER_H

#include <vector>
#include <list>

#include "network/adc/adccommon.h"

namespace Samurai {
namespace IO {
namespace Net {
class Socket;
}
}
}

namespace ADC {

class Command;
class CommandProxy;
class Hub;
class ServiceMap;

enum State { State_None, State_Protocol, State_Identify, State_Verify, State_Normal, State_WaitDisconnect };
enum Credentials { Cred_User, Cred_Operator, Cred_Admin };

class HubConnection {
	public:
		HubConnection(Samurai::IO::Net::Socket*);
		virtual ~HubConnection();
		
		virtual void resetTimer();
		
	protected:
		Samurai::IO::Net::Socket* socket;
		Samurai::TimeStamp* last_act;
};


class HubUser : public HubConnection {
	public:
		HubUser(Samurai::IO::Net::Socket*, Hub*);
		virtual ~HubUser();
		
		bool setINF(ADC::Command* cmd, Hub* hub);

		void addFeature(char* feature);
		void addFeatures(char* features);
		void clearFeatures();
		bool hasFeature(char* feature);
		
		void send(ADC::Command* cmd, bool more = false);
		void read();

		sid_t getSID() const { return sid; }
		enum State getState() const { return state; };
		enum Credentials getCredentials() const { return credentials; }
		
		size_t getSendQueueSize();

	protected:
		
		ADC::CommandProxy* proxy;
		enum State state;
		enum Credentials credentials;
		sid_t sid;
		ADC::Command* command_sup;
		ADC::Command* command_inf;
		char* nick;
		char* cid;
		std::vector<char*> features;
		bool locked;
		

	friend class ADC::Hub;
};

}

#endif // HAVE_QUICKDC_ADCHUBUSER_H
