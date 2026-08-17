/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DROPCHECK_H
#define HAVE_QUICKDC_DROPCHECK_H

#include "quickdc.h"
#include "api/dropverdict.h"

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
		~DropCheck() override;

		DropCheck(const DropCheck&) = delete;
		DropCheck& operator=(const DropCheck&) = delete;

		/**
		 * Ask a DROP server about one family.
		 *
		 * @param baseUrl the family's own name, "https://ipv4.drop.example.org".
		 *        The unprefixed name would leave the family under test to
		 *        whichever connection won, since one connection has one source
		 *        address.
		 * @param tcpPort the port this client advertises, never an ephemeral one.
		 * @param udpPort the port search results arrive on; zero to test neither.
		 * @param localAddress the address to bind, where this host has more than
		 *        one of the family: without it the kernel chooses per
		 *        destination and the answer is about whichever it chose.
		 */
		void start(const std::string& baseUrl, Drop::Family family,
		           uint16_t tcpPort, uint16_t udpPort,
		           const std::optional<Samurai::IO::Net::InetAddress>& localAddress);

		/** Abandon a run in flight. Nothing is reported for it. */
		void cancel();

		/**
		 * Send the results somewhere else from now on.
		 *
		 * The core sets up a listener of its own so that a verdict reaches the
		 * hubs whether or not anything is drawing; a front end replaces it and is
		 * then responsible for doing that itself. Null silences reporting without
		 * stopping a run that is already going.
		 */
		void setListener(DropCheckListener* listener_) { listener = listener_; }

		DropCheckListener* getListener() const { return listener; }

		bool isBusy() const { return state != State::Idle; }

		/** What the last completed run established; ran is false until one has. */
		const DropVerdict& verdict() const { return result; }

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
		 * only between a session being opened and the run ending.
		 *
		 * @return true when 'reply' is to be sent back to 'from'.
		 */
		bool answerStunProbe(std::span<const uint8_t> datagram,
		                     const std::string& fromAddress, uint16_t fromPort,
		                     bool fromIsV6, std::vector<uint8_t>& reply) const;

	protected:
		void EventDropServerInfo(Drop::Client*, const Drop::ServerInfo&) override;
		void EventDropWhoami(Drop::Client*, const Drop::Whoami&) override;
		void EventDropSession(Drop::Client*, const Drop::Session&) override;
		void EventDropInbound(Drop::Client*, const Drop::InboundResult&) override;
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

		void internal_fail(const std::string& reason);
		void internal_finish();

		/** Forget the session and with it the credentials that answer a probe. */
		void internal_disarm();

		DropCheckListener* listener;

		/**
		 * Kept past the end of a run and replaced only by the next one.
		 *
		 * Never destroyed from inside a handler: the handler is called from the
		 * client's own stack, and the HTTP client underneath it owns the socket
		 * whose callback is below that.
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

		Drop::Family family = Drop::Family::IPv4;

		DropVerdict result;
};

}

#endif // HAVE_QUICKDC_DROPCHECK_H
