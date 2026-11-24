/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADC_LOCALHUB_CLAIMCHECK_H
#define HAVE_QUICKDC_ADC_LOCALHUB_CLAIMCHECK_H

#include "quickdc.h"

#include <samurai/io/net/socketevent.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "network/adc/adccommon.h"

namespace Samurai {
	namespace IO {
		namespace Net {
			class Socket;
		}
	}
}

/*
 * How long a client is given to answer, and how long before the same endpoint is
 * asked again.
 *
 * A connection that is going to be accepted is accepted at once; one that is going
 * to be dropped may never be answered at all, so the wait has to end by itself. The
 * interval is what stops a client that keeps asking for connections from being
 * checked on every one of them.
 */
#define QUICKDC_CLAIMCHECK_TIMEOUT_SECONDS 5
#define QUICKDC_CLAIMCHECK_INTERVAL_SECONDS 300

/*
 * How many checks a hub runs at once, and how many endpoints it remembers having
 * checked.
 *
 * Every check is an outbound connection the hub makes because a client asked it to,
 * so the number of them is not the hub's own business to leave unbounded. The record
 * of finished ones is what the interval is read from, and it is bounded too: a busy
 * hub would otherwise accumulate one entry per client per interval.
 */
#define QUICKDC_CLAIMCHECK_CONCURRENT 8
#define QUICKDC_CLAIMCHECK_REMEMBERED 512

namespace ADC {
namespace LocalHub {

class Hub;
class User;

/**
 * Test whether a client that says it can be connected to actually can be.
 *
 * A client announcing TCP4 is asserting something every other client acts on: that a
 * peer told to connect to it will get through. Nothing in the protocol checks that,
 * and a client that is wrong about it takes every transfer with it - worse, since
 * NATT engages only when neither side claims TCP4, so a false claim also suppresses
 * the one mechanism that would have worked.
 *
 * The hub is in a position to check. It already has a connection from the client, so
 * it knows where that client is without being told, and the client's own connect
 * request names the port it expects peers to arrive on.
 *
 * What this is not is a general way to make a hub open connections. Three things
 * bound it, and all three matter:
 *
 *  - The address is the one the hub observed for that client's own connection, never
 *    one the client named. The port does come from the client, but a port on one's
 *    own address is one's own business - the same principle INF_CHECK_IP applies to
 *    the address itself.
 *  - One check per endpoint per interval, and a limit on how many run at once.
 *  - One connection, closed as soon as it is answered. Nothing is sent, and nothing
 *    is read.
 */
class ClaimCheck : public Samurai::IO::Net::SocketEventHandler
{
	public:
		/**
		 * Start a check, or decline to.
		 *
		 * @return null when this check will not be made - too many are running,
		 *         this endpoint was checked recently, the port is not one a peer
		 *         connection listens on, or there is no observed address to use.
		 *         The caller has nothing to wait for and nothing to clean up.
		 */
		static ClaimCheck* start(Hub* hub, User* user, const char* protocol,
			uint16_t port);




		~ClaimCheck() override;

		ClaimCheck(const ClaimCheck&) = delete;
		ClaimCheck& operator=(const ClaimCheck&) = delete;

	protected:
		void EventConnected(const Samurai::IO::Net::Socket*) override;
		void EventDisconnected(const Samurai::IO::Net::Socket*) override;
		void EventError(const Samurai::IO::Net::Socket*,
			Samurai::IO::Net::SocketError, const char*) override;

	private:
		friend class ClaimChecks;

		ClaimCheck(Hub* hub, sid_t sid, const std::string& address, uint16_t port,
			const std::string& protocol);

		/** The claim holds: nothing is said, because nothing is wrong. */
		void reachable();

		/** The claim does not hold, and the client is told so once. */
		void unreachable(const char* why);

		/** Stop, whichever way it went, and stay behind as the interval's record. */
		void finish();

		Hub* hub;

		/*
		 * Who this is about, by session ID rather than by pointer: a check outlives
		 * a client that disconnects while it is running, so a pointer to the user
		 * would be one to something freed. Looked up when there is something to
		 * say, which is also the moment it matters whether they are still here.
		 */
		sid_t sid;

		std::string address;
		uint16_t port;

		/* From the CTM, so the report names which of them could not be made. */
		std::string protocol;

		/*
		 * When this stopped, or nothing while it is still running.
		 *
		 * A finished check is not thrown away at once: it is the record the
		 * interval is read from, so it stays until the interval has passed and
		 * then makes way for the next one.
		 */
		std::optional<std::chrono::steady_clock::time_point> finished;

		std::shared_ptr<Samurai::IO::Net::Socket> socket;
};

/**
 * The checks one hub has made, running and finished both.
 *
 * A check has to outlive the event that ended it - it is told the connection failed
 * by the socket it owns, so it cannot free itself while that call is on the stack -
 * and a finished one is the record the interval is read from. The hub owns this, so
 * two hubs keep two records and neither's checks count against the other's limit.
 */
class ClaimChecks
{
	public:
		ClaimChecks();
		~ClaimChecks();

		ClaimChecks(const ClaimChecks&) = delete;
		ClaimChecks& operator=(const ClaimChecks&) = delete;

		/** How many are running, which is what the concurrency limit applies to. */
		size_t running() const;

		/**
		 * Abandon every one. A check holds the hub it will report to and finishes
		 * seconds after it started, so the hub calls this on its way out rather than
		 * leave one to report into freed memory.
		 */
		void clear();

	private:
		std::vector<std::unique_ptr<ClaimCheck>> checks;
		friend class ClaimCheck;
};

} // namespace LocalHub
} // namespace ADC

#endif // HAVE_QUICKDC_ADC_LOCALHUB_CLAIMCHECK_H
