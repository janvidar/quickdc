/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_PORTFORWARD_H
#define HAVE_QUICKDC_PORTFORWARD_H

#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/upnp/gateway.h>
#include <samurai/io/net/upnp/portmapper.h>

#include <chrono>
#include <memory>
#include <stdint.h>
#include <string>

namespace Samurai {
	namespace IO {
		namespace Net {
			class URL;
		}
	}
}

namespace QuickDC {

/**
 * How long a mapping is asked to last.
 *
 * A finite lease is what cleans up after a client that crashed or was killed:
 * the mapping goes away by itself instead of pointing at whatever later takes
 * the same address on the local network. It is renewed at half of what the
 * gateway grants, so the number only has to be long enough that a renewal is
 * not constant - and short enough that an abandoned mapping does not outlive
 * the address lease it points at.
 */
#define QUICKDC_PORTFORWARD_LEASE_SECONDS 7200

/** Implement this to follow whether the peer port is reachable from outside. */
class PortForwardListener
{
	public:
		virtual ~PortForwardListener() { }

		/**
		 * The peer port is forwarded, and this is where a peer should be told to
		 * reach it.
		 *
		 * The port may not be the one that was asked for: a gateway with the
		 * requested one already taken hands out another, and this is the one that
		 * has to be advertised.
		 */
		virtual void EventPortForwarded(uint16_t externalPort,
		                                const Samurai::IO::Net::InetAddress& externalAddress) = 0;

		/**
		 * The port could not be forwarded, and the client is passive.
		 *
		 * 'reason' is a sentence fit to show whoever is looking at the connection
		 * settings, and is never null. This is an ordinary outcome and not an
		 * error to be surfaced as one: a network with no UPnP gateway, or one
		 * with the feature turned off in the router, reaches here.
		 */
		virtual void EventPortForwardFailure(const char* reason) = 0;
};

/**
 * Asks the local internet gateway to forward the peer port, so that a client
 * behind NAT can be told about by peers rather than only reaching out to them.
 *
 * This is what the difference between active and passive mode is. A passive
 * client cannot be sent a connection, so two passive peers can never transfer
 * to each other at all - which is why a client that can become active should.
 *
 * Both TCP and UDP are forwarded on the same external port, and the port is
 * only reported once both are in place. UDP is not optional: search replies
 * arrive over it, so a client with TCP forwarded and UDP not would appear
 * active, be sent connections, and silently never see a reply to any search it
 * made.
 *
 * A gateway is not a trusted party. It is on the local network, it is commonly
 * an embedded device that has not been updated, and everything it says about
 * the world outside is its own claim - so the external address it reports is
 * carried to the listener as information and is not compared against anything.
 * What bounds the damage is that a mapping is asked for with a finite lease and
 * renewed, rather than left behind permanently.
 */
class PortForward final
	: private Samurai::IO::Net::UPnP::GatewayEventHandler
	, private Samurai::IO::Net::UPnP::PortMapperEventHandler
{
	public:
		/** @param loop what the search, the gateway and the mapper run on. */
		PortForward(Samurai::EventLoop& loop, PortForwardListener* listener);
		~PortForward() override;

		/* Owns a gateway and a mapper that both hold a pointer back to this as
		 * their handler, so a copy would leave two objects answering for one. */
		PortForward(const PortForward&) = delete;
		PortForward& operator=(const PortForward&) = delete;

		/**
		 * Find a gateway by searching the local network, and forward 'port'.
		 *
		 * The listener hears back exactly once per call. A search tells every
		 * host on the segment that this one is here, which is the cost of the
		 * feature and the reason it is asked for rather than assumed.
		 */
		void start(uint16_t port);

		/**
		 * Forward 'port' on the gateway whose description is at 'location',
		 * skipping the search.
		 *
		 * For a network where the gateway is known, or where multicast does not
		 * survive the path between here and it.
		 */
		void start(uint16_t port, const Samurai::IO::Net::URL& location);

		/**
		 * Give up the mappings and stop renewing them.
		 *
		 * Asked for rather than left to the destructor: releasing a mapping means
		 * talking to the gateway, and so needs an event loop that is still being
		 * driven. A client that exits without calling this is covered by the
		 * lease expiring.
		 */
		void stop();

		/**
		 * stop(), then drive the loop until the gateway has answered every
		 * deletion or 'within' has passed. For shutdown, while the loop's other
		 * users are still alive; what is left over the lease cleans up.
		 */
		void release(std::chrono::milliseconds within);

		/** True between start() and the listener hearing back. */
		bool isBusy() const { return busy; }

		/** True while the port is forwarded, so the client is active. */
		bool isForwarded() const { return forwarded; }

		/** Zero until the port is forwarded. */
		uint16_t getExternalPort() const { return external_port; }

		/**
		 * What the gateway said its outside address is, or null if it has not
		 * been established. Null rather than a placeholder: no address is a
		 * different thing from an address that happens to be zero.
		 */
		const Samurai::IO::Net::InetAddress* getExternalAddress() const;

	private:
		void EventGatewayFound(Samurai::IO::Net::UPnP::Gateway*) override;
		void EventGatewayError(Samurai::IO::Net::UPnP::Gateway*,
		                       Samurai::IO::Net::UPnP::Error) override;

		void EventMappingAdded(Samurai::IO::Net::UPnP::PortMapper*,
		                       const Samurai::IO::Net::UPnP::Mapping&) override;
		void EventMappingRemoved(Samurai::IO::Net::UPnP::PortMapper*,
		                         const Samurai::IO::Net::UPnP::MappingKey&) override;
		void EventMappingRenewed(Samurai::IO::Net::UPnP::PortMapper*,
		                         const Samurai::IO::Net::UPnP::Mapping&) override;
		void EventExternalAddress(Samurai::IO::Net::UPnP::PortMapper*,
		                          const Samurai::IO::Net::InetAddress&) override;
		void EventMapperError(Samurai::IO::Net::UPnP::PortMapper*,
		                      Samurai::IO::Net::UPnP::Error,
		                      Samurai::IO::Net::UPnP::DeviceError, uint16_t) override;

		void internal_reset();
		void internal_mapUdp();
		void internal_fail(const std::string& reason);
		void internal_ready();

		Samurai::EventLoop& loop;
		PortForwardListener* listener;

		std::unique_ptr<Samurai::IO::Net::UPnP::Gateway> gateway;
		std::unique_ptr<Samurai::IO::Net::UPnP::PortMapper> mapper;

		/* What stop() let go of, kept until its deletions are answered: a start()
		   right after would otherwise destroy them in flight and leave the old
		   mapping on the gateway. Its events are not this forward's. */
		std::unique_ptr<Samurai::IO::Net::UPnP::Gateway> retired_gateway;
		std::unique_ptr<Samurai::IO::Net::UPnP::PortMapper> retired_mapper;

		uint16_t internal_port = 0;
		uint16_t external_port = 0;
		bool tcp_done = false;
		bool udp_done = false;
		bool busy = false;
		bool forwarded = false;
		bool reported = false;
		/* Unspecified until the gateway has said, which is how samurai itself
		   spells an address that is not known yet. */
		Samurai::IO::Net::InetAddress external_address;
};

/** Where peers should be told to reach this client. */
struct PeerEndpoint
{
	/** False means passive: nothing can be told to connect here. */
	bool active = false;
	std::string address;
	uint16_t port = 0;
};

/**
 * Decide what to advertise to a hub.
 *
 * Kept as a function of its inputs rather than read out of the core at each of
 * the places that needs it, because both protocols advertise this and they have
 * to agree: a client that tells an ADC hub one port and an NMDC hub another is
 * reachable on one and silently not on the other.
 *
 * A forwarded port wins over the configured one. That is the whole point of the
 * distinction - the gateway may have granted a port other than the one asked
 * for, and it is the granted one that a peer can actually reach, so advertising
 * what was configured would send every peer to a port with nothing behind it.
 *
 * An observed address wins over both, and it is worth saying why it outranks the
 * gateway's own answer: a gateway asked for its outside address answers with the
 * address of *its* outside, which behind a carrier-grade NAT is another private
 * address that reaches nobody. Something that watched our packets arrive cannot
 * be wrong about where they came from.
 *
 * @param wanted     whether the user asked to be active at all
 * @param listening  whether there is a socket to receive a connection on
 * @param observedAddress  where something on the outside says it sees us, empty
 *        when nothing has said. ADC hubs report this; NMDC has no way to, so the
 *        NMDC callers pass nothing and rely on the configured value.
 * @param forward    may be null, meaning no attempt is being made to forward
 */
PeerEndpoint peerEndpoint(bool wanted, bool listening,
                          const std::string& configuredAddress,
                          const std::string& observedAddress,
                          uint16_t configuredPort,
                          const PortForward* forward);

/**
 * The same decision for IPv6, which is a shorter one.
 *
 * No gateway is involved and no port is translated: an IPv6 host carries its own
 * global address, so the port advertised is the port bound and there is nothing
 * for a forward to have changed. What is left to decide is whether there is an
 * endpoint at all.
 *
 * There is one only where the address is known. Unlike IPv4 there is no
 * configured value to fall back on and no "0.0.0.0" to hand a hub meaning 'use
 * what you see' - so until something outside has reported where we are, the
 * honest INF is one that claims no IPv6 endpoint rather than one naming an
 * address peers would fail to reach. A measurement supplies it within seconds of
 * start-up and the hubs are told again when it does.
 *
 * @param wanted     whether the user asked to be active at all
 * @param listening  whether the IPv6 peer socket came up; see DCServer::listening()
 * @param observedAddress  where something outside says it sees us on this family,
 *        which is the measurement's own answer where there is one
 * @param port       the port bound, which is the port advertised
 */
PeerEndpoint peerEndpoint6(bool wanted, bool listening,
                           const std::string& observedAddress,
                           uint16_t port);

}

#endif // HAVE_QUICKDC_PORTFORWARD_H
