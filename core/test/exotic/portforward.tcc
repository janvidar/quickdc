/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * QuickDC::PortForward against an internet gateway that only exists for the
 * test, on a loopback port the kernel picks.
 *
 * Driven through start(port, location) rather than the searching form, because a
 * test cannot rely on what answers a multicast search on the machine it runs on -
 * and because that is also the entry point a caller uses when it has the
 * gateway's address from configuration.
 *
 * The cases that matter are the ones where the gateway does not simply agree. A
 * router hands out a different external port than the one asked for, refuses a
 * lease, refuses the port outright, or grants TCP and refuses UDP - and the last
 * of those must not end with the client believing it is reachable, because an
 * active client that cannot receive a search reply looks to its user like a
 * network that returns nothing rather than like a setting that is wrong.
 */

#include "quickdc.h"
#include "network/portforward.h"

#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/net/url.h>

#include <chrono>
#include <map>
#include <memory>
#include <stdio.h>
#include <string>
#include <utility>
#include <vector>

static const int TIMEOUT_MS = 4000;

template<typename Fn>
static bool pump(Fn done, int timeout_ms = TIMEOUT_MS)
{
	Samurai::IO::Net::SocketMonitor* monitor =
		Samurai::IO::Net::SocketMonitor::getInstance();

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);

	while (!done())
	{
		if (std::chrono::steady_clock::now() > deadline) return done();
		monitor->wait(10);
	}
	return true;
}

/* ------------------------------------------------------------------------- */

/* An IGD version 1 device: no AddAnyPortMapping, so a conflict is walked. */
static const char* IGD1 =
	"<?xml version=\"1.0\"?>\n"
	"<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
	" <device>\n"
	"  <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>\n"
	"  <friendlyName>Test Router</friendlyName>\n"
	"  <serviceList>\n"
	"   <service>\n"
	"    <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>\n"
	"    <controlURL>/ctl/IPConn</controlURL>\n"
	"   </service>\n"
	"  </serviceList>\n"
	" </device>\n"
	"</root>\n";

/* Version 2, which is where AddAnyPortMapping exists. */
static const char* IGD2 =
	"<?xml version=\"1.0\"?>\n"
	"<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
	" <device>\n"
	"  <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:2</deviceType>\n"
	"  <friendlyName>Test Router 2</friendlyName>\n"
	"  <serviceList>\n"
	"   <service>\n"
	"    <serviceType>urn:schemas-upnp-org:service:WANIPConnection:2</serviceType>\n"
	"    <controlURL>/ctl/IPConn</controlURL>\n"
	"   </service>\n"
	"  </serviceList>\n"
	" </device>\n"
	"</root>\n";

/* A device that is not a router at all. */
static const char* NOT_A_ROUTER =
	"<?xml version=\"1.0\"?>\n"
	"<root>\n"
	" <device>\n"
	"  <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>\n"
	"  <friendlyName>Not A Router</friendlyName>\n"
	"  <serviceList>\n"
	"   <service>\n"
	"    <serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>\n"
	"    <controlURL>/ctl/Content</controlURL>\n"
	"   </service>\n"
	"  </serviceList>\n"
	" </device>\n"
	"</root>\n";

static std::string soapBody(const std::string& action, const std::string& inner)
{
	return "<?xml version=\"1.0\"?>"
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
		"<u:" + action + "Response"
		" xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
		+ inner +
		"</u:" + action + "Response></s:Body></s:Envelope>";
}

static std::string soapFault(int code)
{
	return "<?xml version=\"1.0\"?>"
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
		"<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>"
		"<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
		"<errorCode>" + std::to_string(code) + "</errorCode>"
		"<errorDescription>refused</errorDescription>"
		"</UPnPError></detail></s:Fault></s:Body></s:Envelope>";
}

/** One scripted answer to one action. */
struct Answer
{
	int status = 200;
	std::string body;
};

static Answer ok(const std::string& body)
{
	Answer answer;
	answer.body = body;
	return answer;
}

static Answer fault(int code)
{
	Answer answer;
	answer.status = 500;
	answer.body = soapFault(code);
	return answer;
}

/*
 * A gateway on the loopback.
 *
 * A GET is answered with the description; a POST with whatever the action named
 * in its SOAPAction header is scripted to. An action with a queue of answers
 * hands them out in order, which is how a refusal followed by an acceptance -
 * a lease fallback, or a conflict walk - gets arranged.
 */
class FakeGateway
	: public Samurai::IO::Net::ServerSocketEventHandler
	, public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::string description;
		std::map<std::string, std::vector<Answer>> scripted;
		std::map<std::string, Answer> repeating;

		/* What the gateway was asked, so a case can assert on the requests. */
		std::vector<std::string> actions;
		std::vector<std::string> bodies;
		bool serve_description = true;

		explicit FakeGateway(const char* canned) : description(canned)
		{
			Samurai::IO::Net::InetSocketAddress any(static_cast<uint16_t>(0));
			server = Samurai::IO::Net::ServerSocket::create(this, any);
			if (!server || server->getFD() == -1) return;
			if (!server->listen()) return;
			port = server->getLocalPort();
		}

		~FakeGateway() override
		{
			live.clear();
			server.reset();
		}

		FakeGateway(const FakeGateway&) = delete;
		FakeGateway& operator=(const FakeGateway&) = delete;

		bool ready() const { return server && port != 0; }

		Samurai::IO::Net::URL location() const
		{
			return Samurai::IO::Net::URL(
				"http://127.0.0.1:" + std::to_string(port) + "/rootDesc.xml");
		}

		void script(const std::string& action, const Answer& answer)
		{
			scripted[action].push_back(answer);
		}

		void always(const std::string& action, const Answer& answer)
		{
			repeating[action] = answer;
		}

		/** How many times an action was asked for. */
		size_t count(const std::string& action) const
		{
			size_t seen = 0;
			for (const std::string& name : actions) if (name == action) seen++;
			return seen;
		}

		/** The body of the n'th call of an action, for asserting on arguments. */
		std::string bodyOf(const std::string& action, size_t which = 0) const
		{
			size_t seen = 0;
			for (size_t n = 0; n < actions.size(); n++)
			{
				if (actions[n] != action) continue;
				if (seen++ == which) return bodies[n];
			}
			return std::string();
		}

	protected:
		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override
		{ }

		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket> socket) override
		{
			socket->setEventHandler(this);
			live.push_back(socket);
			held[socket.get()].clear();
		}

		void EventDataAvailable(const Samurai::IO::Net::Socket* which) override
		{
			Samurai::IO::Net::Socket* socket =
				const_cast<Samurai::IO::Net::Socket*>(which);

			char scratch[8192];
			size_t got = 0;
			std::error_code ec;
			if (socket->read(scratch, sizeof(scratch), got, ec) !=
				Samurai::IO::ReadResult::Ok) return;

			std::string& buffer = held[socket];
			buffer.append(scratch, got);

			const size_t head_end = buffer.find("\r\n\r\n");
			if (head_end == std::string::npos) return;

			/* Nothing is answered before the whole declared body has arrived. */
			const size_t length = declaredLength(buffer.substr(0, head_end));
			if (buffer.size() < head_end + 4 + length) return;

			const std::string head = buffer.substr(0, head_end);
			const std::string body = buffer.substr(head_end + 4, length);
			buffer.clear();

			respond(socket, head, body);
		}

		void EventDisconnected(const Samurai::IO::Net::Socket*) override { }

	private:
		static size_t declaredLength(const std::string& head)
		{
			const size_t at = head.find("Content-Length:");
			if (at == std::string::npos) return 0;

			size_t n = at + 15;
			while (n < head.size() && head[n] == ' ') n++;

			size_t value = 0;
			while (n < head.size() && head[n] >= '0' && head[n] <= '9')
				value = value * 10 + static_cast<size_t>(head[n++] - '0');

			return value;
		}

		static std::string actionOf(const std::string& head)
		{
			const size_t at = head.find("SOAPAction:");
			if (at == std::string::npos) return std::string();

			const size_t hash = head.find('#', at);
			if (hash == std::string::npos) return std::string();

			const size_t quote = head.find('"', hash);
			if (quote == std::string::npos) return std::string();

			return head.substr(hash + 1, quote - hash - 1);
		}

		void respond(Samurai::IO::Net::Socket* socket, const std::string& head,
		             const std::string& body)
		{
			Answer answer;

			if (head.compare(0, 4, "GET ") == 0)
			{
				if (serve_description)
				{
					answer.body = description;
				}
				else
				{
					answer.status = 404;
					answer.body = "no";
				}
			}
			else
			{
				const std::string action = actionOf(head);
				actions.push_back(action);
				bodies.push_back(body);

				auto queued = scripted.find(action);
				if (queued != scripted.end() && !queued->second.empty())
				{
					answer = queued->second.front();
					queued->second.erase(queued->second.begin());
				}
				else if (repeating.count(action))
				{
					answer = repeating[action];
				}
				else
				{
					answer = ok(soapBody(action, ""));
				}
			}

			std::string reply = "HTTP/1.1 " + std::to_string(answer.status) + " "
				+ (answer.status == 200 ? "OK" : "Error") + "\r\n";
			reply += "Content-Type: text/xml; charset=\"utf-8\"\r\n";
			reply += "Content-Length: " + std::to_string(answer.body.size()) + "\r\n";
			reply += "Connection: close\r\n\r\n";
			reply += answer.body;

			std::error_code ec;
			socket->write(reply.data(), reply.size(), ec);

			held.erase(socket);
			for (auto it = live.begin(); it != live.end(); ++it)
			{
				if (it->get() == socket) { live.erase(it); return; }
			}
		}

		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		std::vector<std::shared_ptr<Samurai::IO::Net::Socket>> live;
		std::map<Samurai::IO::Net::Socket*, std::string> held;
		uint16_t port = 0;
};

/* ------------------------------------------------------------------------- */

class Recorder : public QuickDC::PortForwardListener
{
	public:
		int forwarded = 0;
		int failed = 0;
		uint16_t port = 0;
		std::string address;
		std::string reason;

		bool done() const { return forwarded || failed; }

		void EventPortForwarded(uint16_t external,
			const Samurai::IO::Net::InetAddress& external_address) override
		{
			forwarded++;
			port = external;
			address = external_address.toString();
		}

		void EventPortForwardFailure(const char* why) override
		{
			failed++;
			reason = why ? why : "(null)";
		}
};

/** A gateway that grants everything, which is the shape of the happy path. */
static void agreeable(FakeGateway& gateway, uint16_t reserved = 0)
{
	if (reserved)
		gateway.always("AddAnyPortMapping",
			ok(soapBody("AddAnyPortMapping",
				"<NewReservedPort>" + std::to_string(reserved) + "</NewReservedPort>")));

	gateway.always("GetExternalIPAddress",
		ok(soapBody("GetExternalIPAddress",
			"<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>")));
}

/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */

/*
 * What gets advertised to a hub.
 *
 * Both protocols ask this one function, so these are the cases that decide what
 * an ADC INF and an NMDC $MyINFO/$ConnectToMe carry. The forwarded port winning
 * over the configured one is the case the rest of this file exists for.
 */

/* ---- nothing asked for yet ---- */
EXO_TEST(portforward_a_port_forward_that_was_never_started_claims_nothing,
{
		Recorder events;
		QuickDC::PortForward forward(&events);

		EXO_ASSERT(!forward.isBusy());
		EXO_ASSERT(!forward.isForwarded());
		EXO_ASSERT(forward.getExternalPort() == 0);
		EXO_ASSERT(forward.getExternalAddress() == nullptr);
	
	return 1;
});

/*
	 * Version 1, so the port is asked for by name rather than left to the device.
	 * Both protocols have to be mapped before the port is reported, and the
	 * address is what the gateway said.
	 */
EXO_TEST(portforward_both_protocols_are_forwarded_and_the_address_reported,
{
		FakeGateway gateway(IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.forwarded == 1);
		EXO_ASSERT(events.failed == 0);
		EXO_ASSERT(events.port == 1411);
		EXO_ASSERT(events.address == "203.0.113.7");
		EXO_ASSERT(forward.isForwarded());
		EXO_ASSERT(!forward.isBusy());
		EXO_ASSERT(forward.getExternalPort() == 1411);
		EXO_ASSERT(forward.getExternalAddress() != nullptr);

		/* TCP, then UDP, then the address - and nothing else. */
		EXO_ASSERT(gateway.count("AddPortMapping") == 2);
		EXO_ASSERT(gateway.count("GetExternalIPAddress") == 1);
		EXO_ASSERT(gateway.bodyOf("AddPortMapping", 0).find("<NewProtocol>TCP</NewProtocol>")
			!= std::string::npos);
		EXO_ASSERT(gateway.bodyOf("AddPortMapping", 1).find("<NewProtocol>UDP</NewProtocol>")
			!= std::string::npos);
	
	return 1;
});

/*
	 * The case the whole design turns on. A version 2 device may grant a port
	 * other than the one asked for, and the UDP mapping has to follow it: a peer
	 * is told one number, so a search reply arriving on another is lost with
	 * nothing anywhere to say why.
	 */
EXO_TEST(portforward_a_moved_external_port_is_carried_to_the_udp_mapping_too,
{
		FakeGateway gateway(IGD2);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway, 40001);

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.forwarded == 1);
		EXO_ASSERT(events.port == 40001);
		EXO_ASSERT(forward.getExternalPort() == 40001);

		/* The UDP mapping named the port TCP was actually granted, and still
		   forwards to the port this client is listening on. */
		const std::string udp = gateway.bodyOf("AddPortMapping", 0);
		EXO_ASSERT(gateway.count("AddAnyPortMapping") == 1);
		EXO_ASSERT(gateway.count("AddPortMapping") == 1);
		EXO_ASSERT(udp.find("<NewProtocol>UDP</NewProtocol>") != std::string::npos);
		EXO_ASSERT(udp.find("<NewExternalPort>40001</NewExternalPort>") != std::string::npos);
		EXO_ASSERT(udp.find("<NewInternalPort>1411</NewInternalPort>") != std::string::npos);
	
	return 1;
});

/*
	 * 725 is common enough that samurai retries without a lease rather than
	 * reporting it. Asserted here because this component asks for a lease, and a
	 * client on such a router has to end up forwarded rather than passive.
	 */
EXO_TEST(portforward_a_router_that_refuses_a_lease_is_asked_again_without_one,
{
		FakeGateway gateway(IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);
		gateway.script("AddPortMapping", fault(725));

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.forwarded == 1);
		EXO_ASSERT(events.failed == 0);
		EXO_ASSERT(events.port == 1411);

		/* The refused attempt, the one without a lease, and then UDP. */
		EXO_ASSERT(gateway.count("AddPortMapping") == 3);
		EXO_ASSERT(gateway.bodyOf("AddPortMapping", 1)
			.find("<NewLeaseDuration>0</NewLeaseDuration>") != std::string::npos);
	
	return 1;
});

/* ---- a port somebody else already holds ---- */
EXO_TEST(portforward_a_taken_port_is_walked_past_and_udp_follows,
{
		FakeGateway gateway(IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);
		/* Refused once; the walk to the next port is what addAnyMapping promises. */
		gateway.script("AddPortMapping", fault(718));

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.forwarded == 1);
		EXO_ASSERT(events.port == 1412);

		/* And UDP followed it rather than staying on the port that was refused. */
		const std::string udp = gateway.bodyOf("AddPortMapping", 2);
		EXO_ASSERT(udp.find("<NewProtocol>UDP</NewProtocol>") != std::string::npos);
		EXO_ASSERT(udp.find("<NewExternalPort>1412</NewExternalPort>") != std::string::npos);
	
	return 1;
});

/*
	 * Reported as a failure on purpose. A client with TCP forwarded and no UDP
	 * would be told it is active, be sent connections, and never see a reply to
	 * a search it made - which presents to its user as a network with nothing on
	 * it rather than as something to fix.
	 */
EXO_TEST(portforward_tcp_alone_is_not_reported_as_forwarded,
{
		FakeGateway gateway(IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);
		gateway.script("AddPortMapping", ok(soapBody("AddPortMapping", "")));
		gateway.script("AddPortMapping", fault(718));

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.failed == 1);
		EXO_ASSERT(events.forwarded == 0);
		EXO_ASSERT(!forward.isForwarded());
		EXO_ASSERT(events.reason.find("718") != std::string::npos);
	
	return 1;
});

/* ---- a device that cannot forward anything ---- */
EXO_TEST(portforward_a_device_with_no_connection_service_is_refused,
{
		FakeGateway gateway(NOT_A_ROUTER);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.failed == 1);
		EXO_ASSERT(!forward.isForwarded());
		EXO_ASSERT(gateway.count("AddPortMapping") == 0);
	
	return 1;
});

/* ---- a description that cannot be read ---- */
EXO_TEST(portforward_a_description_that_cannot_be_fetched_is_refused,
{
		FakeGateway gateway(IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		gateway.serve_description = false;

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.failed == 1);
		EXO_ASSERT(!forward.isForwarded());
	
	return 1;
});

/* ---- releasing ---- */
EXO_TEST(portforward_stop_releases_both_mappings_and_reports_nothing,
{
		FakeGateway gateway(IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());
		pump([&] { return events.done(); });
		EXO_ASSERT(events.forwarded == 1);

		forward.stop();
		pump([&] { return gateway.count("DeletePortMapping") >= 2; });

		EXO_ASSERT(gateway.count("DeletePortMapping") == 2);
		EXO_ASSERT(!forward.isForwarded());
		EXO_ASSERT(forward.getExternalPort() == 0);
		/* Nothing further is reported: the caller asked for this. */
		EXO_ASSERT(events.forwarded == 1);
		EXO_ASSERT(events.failed == 0);
	
	return 1;
});

/* ---- asked twice ---- */
EXO_TEST(portforward_a_second_start_while_one_is_in_flight_is_refused,
{
		FakeGateway gateway(IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());
		EXO_ASSERT(forward.isBusy());

		forward.start(1411, gateway.location());
		EXO_ASSERT(events.failed == 1);
		EXO_ASSERT(events.reason.find("already") != std::string::npos);

		pump([&] { return events.forwarded != 0; });
	
	return 1;
});

/*
	 * Nothing is reported and nothing is left registered with the monitor: the
	 * gateway and the mapper both hold a pointer back to the object being
	 * destroyed.
	 */
EXO_TEST(portforward_a_port_forward_destroyed_in_flight_reports_nothing,
{
		FakeGateway gateway(IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);

		Recorder events;
		{
			QuickDC::PortForward forward(&events);
			forward.start(1411, gateway.location());
			pump([&] { return gateway.count("AddPortMapping") >= 1; });
		}

		pump([] { return false; }, 200);

		EXO_ASSERT(events.forwarded == 0);
		EXO_ASSERT(events.failed == 0);
	
	return 1;
});

/* Asked for, but nothing is listening, so there is nothing to reach. */
EXO_TEST(portforward_active_mode_without_a_listening_socket_is_still_passive,
{
		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, false, "10.0.0.5", 1411, nullptr);
		EXO_ASSERT(!e.active);
	
	return 1;
});

/* Asked for, listening, and not forwarding: what was configured. */
EXO_TEST(portforward_without_forwarding_the_configured_address_is_advertised,
{
		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, true, "10.0.0.5", 1411, nullptr);
		EXO_ASSERT(e.active);
		EXO_ASSERT(e.address == "10.0.0.5");
		EXO_ASSERT(e.port == 1411);
	
	return 1;
});

/* Forwarding attempted and failed: still the configured values, because a
	   user who set up a mapping by hand is entitled to be believed. */
EXO_TEST(portforward_a_failed_forward_leaves_the_configured_address_alone,
{
		FakeGateway gateway(NOT_A_ROUTER);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());
		pump([&] { return events.done(); });
		EXO_ASSERT(events.failed == 1);

		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, true, "10.0.0.5", 1411, &forward);
		EXO_ASSERT(e.active);
		EXO_ASSERT(e.address == "10.0.0.5");
		EXO_ASSERT(e.port == 1411);
	
	return 1;
});

/*
	 * Forwarded, and the gateway moved the port. This is the case that would
	 * otherwise send every peer to a port with nothing behind it.
	 */
EXO_TEST(portforward_a_forwarded_mapping_advertises_the_granted_port_and_outside_address,
{
		FakeGateway gateway(IGD2);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway, 40001);

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());
		pump([&] { return events.done(); });
		EXO_ASSERT(events.forwarded == 1);

		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, true, "10.0.0.5", 1411, &forward);
		EXO_ASSERT(e.active);
		/* The gateway's outside address, not this host's on the LAN. */
		EXO_ASSERT(e.address == "203.0.113.7");
		EXO_ASSERT(e.port == 40001);
	
	return 1;
});

/* Released again: back to what was configured. */
EXO_TEST(portforward_a_released_mapping_falls_back_to_the_configured_port,
{
		FakeGateway gateway(IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);

		Recorder events;
		QuickDC::PortForward forward(&events);
		forward.start(1411, gateway.location());
		pump([&] { return events.done(); });
		forward.stop();

		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, true, "10.0.0.5", 1411, &forward);
		EXO_ASSERT(e.active);
		EXO_ASSERT(e.port == 1411);
	
	return 1;
});
