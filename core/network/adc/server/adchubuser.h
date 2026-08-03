/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCHUBUSER_H
#define HAVE_QUICKDC_ADCHUBUSER_H

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <list>

#include <samurai/timestamp.h>

#include "network/adc/adccommon.h"

namespace Samurai {
namespace IO {
namespace Net {
class Socket;
}
}
}

namespace ADC {

/**
 * The most features one user may advertise in the SU field of its INF.
 *
 * The field is a list the user writes, and the hub consults the whole set once
 * per recipient of every feature cast - so an unbounded list is an unbounded
 * amount of work for anyone else's search.
 */
inline constexpr size_t MAX_USER_FEATURES = 64;

/**
 * Inbound commands one user may have processed: a sustained rate, and a burst it
 * may spend at once. A client answering a search sends its results in one go,
 * which is what the burst is sized for.
 */
inline constexpr double HUB_COMMAND_RATE = 50.0;
inline constexpr double HUB_COMMAND_BURST = 200.0;


class Command;
class CommandProxy;
class Hub;
class ServiceMap;

enum State { State_None, State_Protocol, State_Identify, State_Verify, State_Normal, State_WaitDisconnect };
enum Credentials { Cred_User, Cred_Operator, Cred_Admin };

class HubConnection {
	public:
		HubConnection(std::shared_ptr<Samurai::IO::Net::Socket>);
		virtual ~HubConnection();

		virtual void resetTimer();

		/** Seconds since anything was read from or written to this connection. */
		time_t getIdleTime();

	protected:
		std::shared_ptr<Samurai::IO::Net::Socket> socket;

		/* Held by value: a connection is retired from inside its own socket
		   callbacks, where there is no second chance to release anything. */
		Samurai::TimeStamp last_act;
};


class HubUser : public HubConnection {
	public:
		HubUser(std::shared_ptr<Samurai::IO::Net::Socket>, Hub*);
		virtual ~HubUser();
		
		bool setINF(ADC::Command* cmd, Hub* hub);

		void addFeature(const std::string& feature);
		void addFeatures(const char* features);
		void clearFeatures();
		bool hasFeature(const char* feature) const;
		size_t countFeatures() const { return features.size(); }

		/**
		 * Spend one token from this user's inbound allowance.
		 *
		 * @return false when the allowance is exhausted, which is the hub's cue
		 *         to drop the connection rather than go on doing work for it.
		 */
		bool allowCommand();

		/**
		 * Queue a command for this user.
		 * @return false if the send queue ceiling was reached and the command was
		 *         dropped, which means the user is not keeping up at all.
		 */
		bool send(ADC::Command* cmd, bool more = false);
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

		/*
		 * A set rather than a list of pointers: hasFeature() is asked once per
		 * include and exclude term of every feature cast, for every user on the
		 * hub, and a linear scan over a list the user itself decides the length
		 * of turns one search into work proportional to that length.
		 */
		std::set<std::string> features;
		bool locked;

		/* Inbound command allowance. See allowCommand(). */
		double tokens;
		std::chrono::steady_clock::time_point tokens_filled;

		/** A deferred disconnect is already queued for this user. */
		bool disconnecting;

		/**
		 * Quit bookkeeping, kept apart from 'state' because the quit may be
		 * deferred: state moves to State_WaitDisconnect immediately, so by the
		 * time the deferred pass runs it can no longer tell whether the user had
		 * been visible to anyone and owes them an IQUI.
		 */
		bool quitting;
		bool announce_quit;


	friend class ADC::Hub;
};

}

#endif // HAVE_QUICKDC_ADCHUBUSER_H
