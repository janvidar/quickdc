/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <samurai/io/net/url.h>

#include "quickdc.h"
#include "debug/dbg.h"
#include "network/portforward.h"

namespace {

using namespace Samurai::IO::Net::UPnP;

/**
 * What a peer sees in the gateway's mapping table.
 *
 * Named rather than left blank because this is the list a person reads when
 * working out what on their network asked for what, and an unlabelled entry is
 * one nobody can attribute or safely remove.
 */
const char* MAPPING_DESCRIPTION = PRODUCT " peer connections";

Mapping peerMapping(Protocol protocol, uint16_t external, uint16_t internal_)
{
	Mapping mapping;
	mapping.protocol = protocol;
	mapping.externalPort = external;
	mapping.internalPort = internal_;
	mapping.description = MAPPING_DESCRIPTION;
	mapping.lease = std::chrono::seconds(QUICKDC_PORTFORWARD_LEASE_SECONDS);
	/* internalClient is deliberately left unset: the PortMapper fills it in with
	   the address that faces the gateway, which is the only thing that knows
	   which of this host's addresses reaches it. */
	return mapping;
}

/** Why a gateway could not be used, in a form fit to show a person. */
std::string describeError(Error error)
{
	switch (error)
	{
		case Error::NoGateway:
			return "no internet gateway on this network answered, so the port "
			       "cannot be opened automatically";

		case Error::DescriptionFailed:
			return "the internet gateway answered but its description could not "
			       "be read";

		case Error::NoService:
			return "the internet gateway does not offer a connection service to "
			       "forward a port through";

		case Error::Unsupported:
			return "the internet gateway does not support forwarding a port this "
			       "way";

		case Error::Network:
			return "the exchange with the internet gateway failed";

		case Error::BadResponse:
			return "the internet gateway answered with something that could not "
			       "be read";

		case Error::Cancelled:
			return "opening the port was abandoned";

		default:
			break;
	}

	std::string reason = "the port could not be opened: ";
	reason += toString(error);
	return reason;
}

}


QuickDC::PortForward::PortForward(Samurai::EventLoop& loop_, PortForwardListener* listener_)
	: loop(loop_), listener(listener_)
{
}


QuickDC::PortForward::~PortForward()
{
	/* Deliberately not stop(): releasing a mapping means talking to the gateway,
	   which needs an event loop that is still running. The lease is what covers
	   a client that went away without asking. Both are dropped in the order they
	   depend on, since the mapper borrows the gateway. */
	mapper.reset();
	gateway.reset();
}


const Samurai::IO::Net::InetAddress* QuickDC::PortForward::getExternalAddress() const
{
	if (external_address.getType() == Samurai::IO::Net::InetAddress::Version::Unspecified)
		return nullptr;
	return &external_address;
}


void QuickDC::PortForward::internal_reset()
{
	mapper.reset();
	gateway.reset();

	external_port = 0;
	tcp_done = false;
	udp_done = false;
	forwarded = false;
	reported = false;
	external_address = Samurai::IO::Net::InetAddress();
}


void QuickDC::PortForward::start(uint16_t port)
{
	if (busy) { internal_fail("a port is already being opened"); return; }

	internal_reset();
	internal_port = port;
	busy = true;

	QDBG("Searching for an internet gateway to forward port %d", static_cast<int>(port));
	gateway = Gateway::discover(loop, this);
}


void QuickDC::PortForward::start(uint16_t port, const Samurai::IO::Net::URL& location)
{
	if (busy) { internal_fail("a port is already being opened"); return; }

	internal_reset();
	internal_port = port;
	busy = true;

	QDBG("Asking the gateway at %s to forward port %d",
		location.toString().c_str(), static_cast<int>(port));
	gateway = Gateway::describe(loop, this, location);
}


void QuickDC::PortForward::stop()
{
	if (mapper)
	{
		/* Releases what was added and stops the renewals; the answers are of no
		   interest, since nothing is being reported after this. */
		reported = true;
		mapper->shutdown();
	}

	busy = false;
	forwarded = false;
	external_port = 0;
	tcp_done = false;
	udp_done = false;
}


void QuickDC::PortForward::internal_fail(const std::string& reason)
{
	if (reported) return;
	reported = true;

	busy = false;
	forwarded = false;

	QERR("Port forwarding: %s", reason.c_str());
	listener->EventPortForwardFailure(reason.c_str());
}


void QuickDC::PortForward::internal_ready()
{
	if (reported) return;
	reported = true;

	busy = false;
	forwarded = true;

	QDBG("Port %d is forwarded to %d",
		static_cast<int>(internal_port), static_cast<int>(external_port));
	listener->EventPortForwarded(external_port, external_address);
}


void QuickDC::PortForward::EventGatewayFound(Gateway*)
{
	QDBG("Found the internet gateway '%s', IGD version %d",
		gateway->getFriendlyName().c_str(), gateway->getDeviceVersion());

	PortMapper::Options options;
	options.lease = std::chrono::seconds(QUICKDC_PORTFORWARD_LEASE_SECONDS);

	mapper = PortMapper::create(loop, this, gateway.get(), options);
	if (!mapper)
	{
		internal_fail("the internet gateway could not be asked to forward a port");
		return;
	}

	/*
	 * The external port is asked for rather than demanded, because the one this
	 * client wants is commonly already spoken for - by another client on the
	 * network, or by a mapping an earlier run left behind. Whatever is granted
	 * is what gets advertised.
	 */
	mapper->addAnyMapping(peerMapping(Protocol::TCP, internal_port, internal_port));
}


void QuickDC::PortForward::EventGatewayError(Gateway*, Error error)
{
	internal_fail(describeError(error));
}


void QuickDC::PortForward::internal_mapUdp()
{
	/*
	 * Pinned to the port TCP was granted rather than asked for freely: a peer is
	 * told one number, and a search reply arriving on a different one would be
	 * lost with nothing to say so.
	 */
	mapper->addMapping(peerMapping(Protocol::UDP, external_port, internal_port));
}


void QuickDC::PortForward::EventMappingAdded(PortMapper*, const Mapping& mapping)
{
	if (mapping.protocol == Protocol::TCP)
	{
		external_port = mapping.externalPort;
		tcp_done = true;

		if (external_port != internal_port)
			QDBG("The gateway moved the port from %d to %d",
				static_cast<int>(internal_port), static_cast<int>(external_port));

		internal_mapUdp();
		return;
	}

	udp_done = true;

	/*
	 * Only asked for once both mappings exist, which is what makes a refusal of
	 * either one end as a failure: a refusal reports before this is reached, and
	 * nothing else asks for the address.
	 */
	mapper->getExternalAddress();
}


void QuickDC::PortForward::EventExternalAddress(PortMapper*,
                                                const Samurai::IO::Net::InetAddress& address)
{
	external_address = address;

	/* Both flags are true by the time the address was even asked for; tested
	   again only so that an answer arriving out of order cannot report a port as
	   open before it is. */
	if (tcp_done && udp_done) internal_ready();
}


void QuickDC::PortForward::EventMappingRenewed(PortMapper*, const Mapping& mapping)
{
	QDBG("Renewed the %s mapping on port %d",
		toString(mapping.protocol), static_cast<int>(mapping.externalPort));
	(void) mapping;
}


void QuickDC::PortForward::EventMappingRemoved(PortMapper*, const MappingKey& key)
{
	QDBG("Released the %s mapping on port %d",
		toString(key.protocol), static_cast<int>(key.externalPort));
	(void) key;
}


void QuickDC::PortForward::EventMapperError(PortMapper*, Error error,
                                            DeviceError device, uint16_t code)
{
	if (error != Error::Device)
	{
		internal_fail(describeError(error));
		return;
	}

	/*
	 * The device answered and refused. The code is worth carrying because it is
	 * the difference between something to change and something to accept: a port
	 * already spoken for can be worked around, a gateway that will not authorise
	 * the request at all cannot.
	 */
	std::string reason = "the internet gateway refused to forward the port: ";
	reason += toString(device);
	reason += " (";
	reason += std::to_string(code);
	reason += ")";
	internal_fail(reason);
}


QuickDC::PeerEndpoint QuickDC::peerEndpoint(bool wanted, bool listening,
                                            const std::string& configuredAddress,
                                            const std::string& observedAddress,
                                            uint16_t configuredPort,
                                            const PortForward* forward)
{
	PeerEndpoint endpoint;

	/* Being asked for is not enough: without a socket there is nothing for a
	   peer to reach, whatever the gateway was told. */
	if (!wanted || !listening) return endpoint;

	endpoint.active = true;

	/*
	 * Least authoritative first, so that each better answer overwrites the one
	 * before it and the order on the page is the order of precedence. Written as a
	 * sequence rather than as early returns because the address and the port are
	 * decided by different things: a hub knows where we are but not what the
	 * gateway granted, and the gateway knows the port but not, behind a
	 * carrier-grade NAT, any address that reaches us.
	 */
	endpoint.address = configuredAddress;
	endpoint.port = configuredPort;

	if (forward && forward->isForwarded())
	{
		endpoint.port = forward->getExternalPort();

		/*
		 * The configured address is kept when the gateway did not say what its
		 * outside address is: the port is the part that has to be right, and an
		 * address of "0.0.0.0" is understood by a hub to mean "use the address you
		 * see me coming from", which is better than a blank.
		 */
		if (const Samurai::IO::Net::InetAddress* outside = forward->getExternalAddress())
			endpoint.address = outside->toString();
	}

	/* Best, so last: see the note on the declaration. */
	if (!observedAddress.empty())
		endpoint.address = observedAddress;

	return endpoint;
}


QuickDC::PeerEndpoint QuickDC::peerEndpoint6(bool wanted, bool listening,
                                             const std::string& observedAddress,
                                             uint16_t port)
{
	PeerEndpoint endpoint;

	if (!wanted || !listening) return endpoint;

	/* Nothing to advertise until something outside has said where we are; see
	   the note on the declaration for why there is nothing to fall back on. */
	if (observedAddress.empty()) return endpoint;

	endpoint.active = true;
	endpoint.address = observedAddress;
	endpoint.port = port;

	return endpoint;
}
