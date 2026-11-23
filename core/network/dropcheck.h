/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DROPCHECK_H
#define HAVE_QUICKDC_DROPCHECK_H

#include "quickdc.h"
#include "api/dropverdict.h"
#include "api/subscription.h"

#include <drop/client.h>
#include <drop/types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Samurai {
	namespace IO {
		namespace Net {
			class InetAddress;
		}
	}
}

namespace QuickDC {

/**
 * Measures whether anything outside can reach the port this client advertises.
 *
 * A DROP server is asked what address it sees us at, and then to connect back
 * to the port we are already listening on. Reachability cannot be established
 * any other way: what arrives unsolicited from outside is the only evidence
 * there is, and Reachability beside this waits for that to happen by itself.
 * This asks for it.
 *
 * Event-driven, and deliberately not Drop::check(): that one drives the socket
 * monitor itself, which from inside this reactor would dispatch the event loop
 * re-entrantly and free sockets whose callbacks are on the stack. Drop::Client
 * is asked one request at a time instead and this advances on each answer, so a
 * run costs the loop nothing but the turns it would have taken anyway.
 *
 * The port tested is the one in the client's own INF and nothing else. A check
 * of some ephemeral port would be true about a port nobody will ever connect to.
 */
class DropCheck final : public Drop::ClientEventHandler
{
	public:
		explicit DropCheck(DropCheckListener* listener);

		/**
		 * Ends a run in flight, and takes the client with it.
		 *
		 * Not through cancel(), which reports nothing but leaves the client in
		 * place. Here the client goes too, and the order matters only in that the
		 * credentials stop answering before the socket that carried them can be
		 * read again.
		 */
		~DropCheck() override;

		DropCheck(const DropCheck&) = delete;
		DropCheck& operator=(const DropCheck&) = delete;

		/**
		 * Ask a DROP server about one family.
		 *
		 * The run opens by asking for the server's capabilities, which is also
		 * what says this is a DROP server rather than something that answers 200
		 * to everything, and what carries the bounds the rest of the run has to
		 * stay inside. A client left from an earlier run is replaced in one step
		 * rather than reset first, since this is reached from a handler whenever a
		 * check is asked for by one.
		 *
		 * @param baseUrl the family's own name, "https://ipv4.drop.example.org".
		 *        The unprefixed name would leave the family under test to
		 *        whichever connection won, since one connection has one source
		 *        address.
		 * @param tcpPort the port this client advertises, never an ephemeral one.
		 * @param udpPort the port search results arrive on; zero to test neither.
		 * @param localAddress the address to bind, where this host has more than
		 *        one of the family: without it the kernel chooses per destination,
		 *        and the server would report, and probe, whichever address it
		 *        happened to be reached from rather than the one this client
		 *        advertises.
		 */
		void start(const std::string& baseUrl, Drop::Family family,
		           uint16_t tcpPort, uint16_t udpPort,
		           const std::optional<Samurai::IO::Net::InetAddress>& localAddress);

		/**
		 * Ask a DROP server what address it sees, and nothing further.
		 *
		 * The first two requests of a check and then the end of the run: no session
		 * is opened, so nothing is ever armed to answer a stranger, and no port is
		 * probed. That is what makes it the right question for a client which is not
		 * trying to be reachable - a passive client is still behind an address, and
		 * the address is the part it can be shown without a third party dialling a
		 * port nobody is offering.
		 *
		 * No peer port is asked for, and none is needed: start() refuses without one
		 * because it would otherwise measure a port nothing listens on, while a
		 * lookup has no port to offer in the first place.
		 *
		 * The verdict left behind carries the address and the server's
		 * classification of it, and is marked addressOnly so that nothing mistakes
		 * it for a measurement; see QuickDC::DropVerdict.
		 *
		 * @param baseUrl the family's own name, as start() takes it and for the same
		 *        reason: one connection has one source address.
		 * @param localAddress the address to bind, where this host has more than one
		 *        of the family; without it the kernel chooses per destination and the
		 *        server reports whichever it was reached from.
		 */
		void startAddressLookup(const std::string& baseUrl, Drop::Family family,
		                        const std::optional<Samurai::IO::Net::InetAddress>& localAddress);

		/** Abandon a run in flight. Nothing is reported for it. */
		void cancel();

		/**
		 * Also report to 'observer_' from now on, or to nobody when null.
		 *
		 * Beside the core's own listener rather than in place of it. What the
		 * core does with a verdict - record the measurement, ask the gateway to
		 * forward a port that nothing reached, have the hubs republish - is not
		 * a front end's to take over by drawing the same answer, and a client
		 * whose behaviour depended on whether a window happened to be open would
		 * be two different clients.
		 *
		 * The core is told first, so that anything reading the record when the
		 * front end draws sees the measurement already in it. Both are given the
		 * same copy rather than the member itself, so that a verdict cannot be
		 * overwritten underneath the second one by a run the first started.
		 */
		Subscription observe(DropCheckListener* observer_) { return observers.subscribe(observer_); }

		/** The single observer of the old interface; replaces the previous one. */
		void setObserver(DropCheckListener* observer_)
		{
			observers.remove(observer);
			observer = observer_;
			observers.add(observer);
		}

		DropCheckListener* getObserver() const { return observer; }

		bool isBusy() const { return state != State::Idle; }

		/** What the last completed run established; ran is false until one has. */
		/**
		 * What the last completed run for 'ipv6' established.
		 *
		 * One per family, because a run answers for the family it asked about
		 * and the answers routinely differ - native IPv6 and an IPv4 address
		 * behind a carrier-grade NAT is the ordinary home connection. A single
		 * slot would leave whichever ran last describing both.
		 *
		 * 'ran' is false where that family has not been measured, which is not
		 * the same as having been measured and found unreachable.
		 */
		const DropVerdict& verdict(bool ipv6) const { return ipv6 ? held_v6 : held_v4; }

		/** Which family the run that just ended concerned; see startDropCheck(). */
		bool lastFamilyWasIPv6() const { return family == Drop::Family::IPv6; }

		/**
		 * Answer a DROP challenge that arrived on the peer port.
		 *
		 * Called from the protocol probe, which has just read bytes that are
		 * neither ADC nor a TLS handshake. Answers only while a run is waiting
		 * to be connected back to: outside that window there is nothing this
		 * could be an answer to, and a client that echoes the challenge to
		 * anybody is telling strangers what it runs for nothing in return.
		 *
		 * @return true when 'response' is to be written back.
		 */
		bool answerChallenge(std::string_view line, std::string& response) const;

		/**
		 * Answer an authenticated STUN probe that arrived on the UDP port.
		 *
		 * The credentials are the security boundary, not politeness: a reply to
		 * a datagram whose source was never proven is a reflection an attacker
		 * aims by spoofing it, so this answers nothing at all unless a session
		 * is open and the request verifies against it. The credentials are held
		 * only between a session being opened and the run ending, and the empty
		 * credentials of a run that is not open are themselves the refusal, since
		 * the library answers nothing without a password. The run's state is
		 * checked here as well, so that the rule is visible where it is relied
		 * upon rather than only inside the library.
		 *
		 * @return true when 'reply' is to be sent back to 'from'.
		 */
		bool answerStunProbe(std::span<const uint8_t> datagram,
		                     const std::string& fromAddress, uint16_t fromPort,
		                     bool fromIsV6, std::vector<uint8_t>& reply) const;

	protected:
		/**
		 * Take what the server offers, and hold the run to it.
		 *
		 * The UDP test is asked for only where the server runs it for the family
		 * under test. The two lists routinely differ - the second address RFC 5780
		 * needs is commonly there on one family and not the other - and asking for
		 * a test a server does not run is a refusal rather than an answer. A port
		 * the server will not probe ends the run here instead, which is better
		 * than reporting the refusal that would follow: the bound is the server's,
		 * so nothing this client does about its own configuration would make the
		 * answer arrive.
		 */
		void EventDropServerInfo(Drop::Client*, const Drop::ServerInfo&) override;

		/**
		 * Take the address the server saw us at, and open a session.
		 *
		 * What is kept is the server's own wording where it gave one and its bare
		 * classification where it did not, so that what a user is shown is what
		 * the measurer said. Nothing is armed yet, because the listeners this asks
		 * about are the ones already up: the peer port and the search port are
		 * bound for as long as the client runs. What a session adds is the
		 * credentials that let a probe be told apart from a stranger.
		 *
		 * An address lookup has what it came for here and finishes instead of
		 * opening that session, which is the whole of the difference between the
		 * two runs: no credentials are ever held, so no window in which this
		 * client answers a stranger is opened by one.
		 */
		void EventDropWhoami(Drop::Client*, const Drop::Whoami&) override;

		/**
		 * Arm the listeners, and ask for the probes.
		 *
		 * Armed before the probes are asked for and never after: the server
		 * connects back while the request is outstanding, so a client that asks
		 * first races the probe against its own willingness to answer it.
		 */
		void EventDropSession(Drop::Client*, const Drop::Session&) override;

		/**
		 * Turn the probe results into the verdict the run reports.
		 *
		 * The mode and the per-port outcomes come from the library's own decision
		 * table rather than from a reading of the verdicts here. It lives in one
		 * place on purpose: a user who moves between two clients and is given two
		 * different explanations of the same network concludes that one of them is
		 * broken.
		 */
		void EventDropInbound(Drop::Client*, const Drop::InboundResult&) override;

		/**
		 * Give up on the run, and say why.
		 *
		 * Reached from inside a request as well as from the wire: a request
		 * refused before it reaches a socket reports itself from the call that
		 * made it, which is why every state is set before its request rather than
		 * after.
		 */
		void EventDropError(Drop::Client*, const Drop::ErrorInfo&) override;

	private:
		/**
		 * Where the run has got to.
		 *
		 * Set before each request rather than after, because a request that is
		 * refused before it reaches the wire - one asked while another is in
		 * flight, or a base URL that is not https - reports its error from
		 * inside the call, so the handler can run before the call returns.
		 */
		enum class State { Idle, ServerInfo, Whoami, Session, Inbound };

		/**
		 * Build the client and ask for the server's capabilities.
		 *
		 * What the two entry points have in common, which is everything from the
		 * first request onwards: they differ only in what they establish before it
		 * and in where the run stops. Shared rather than repeated so that a run
		 * cannot be announced by one and not the other, or begin at a different
		 * request depending on which was called.
		 */
		void internal_begin(const std::string& baseUrl,
		                    const std::optional<Samurai::IO::Net::InetAddress>& localAddress);

		void internal_fail(const std::string& reason);

		/**
		 * Report what a completed run established.
		 *
		 * The session is forgotten before the verdict goes out rather than after,
		 * so that nothing can observe a client still willing to answer a probe for
		 * a run that is over.
		 */
		void internal_finish();

		/** Forget the session and with it the credentials that answer a probe. */
		void internal_disarm();

		/**
		 * The core's own, given at construction and never replaced: what the core
		 * does with a verdict happens whether or not anything is drawing.
		 */
		DropCheckListener* listener;

		/** Front ends', beside the above rather than instead of it. */
		ListenerList<DropCheckListener> observers;

		/** The one setObserver() put in 'observers'. */
		DropCheckListener* observer = nullptr;

		/**
		 * Kept past the end of a run and replaced only by the next one.
		 *
		 * Never destroyed from inside a handler: the handler is called from the
		 * client's own stack, and the HTTP client underneath it owns the socket
		 * whose callback is below that. A new run therefore replaces it in one
		 * step instead of resetting it first.
		 */
		std::unique_ptr<Drop::Client> client;

		State state = State::Idle;

		/* What the run has established so far. */
		Drop::ServerInfo server;
		Drop::Whoami whoami;
		Drop::Session session;

		/** True between openSession() answering and the run ending. */
		bool armed = false;

		uint16_t tcp_port = 0;
		uint16_t udp_port = 0;

		/** Whether this server offers the UDP test for the family under test. */
		bool want_udp = false;

		/**
		 * Whether the run in progress stops once the address is known.
		 *
		 * Kept for the run rather than asked of the caller again, because the
		 * question is answered in one handler and by then the call that started
		 * things has long returned; see startAddressLookup().
		 */
		bool address_only = false;

		Drop::Family family = Drop::Family::IPv4;

		/** The run in progress, which is copied into its family's slot when it ends. */
		DropVerdict result;

		DropVerdict held_v4;
		DropVerdict held_v6;
};

}

#endif // HAVE_QUICKDC_DROPCHECK_H
