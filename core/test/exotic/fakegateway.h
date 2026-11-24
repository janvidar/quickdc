/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_TEST_FAKEGATEWAY_H
#define HAVE_QUICKDC_TEST_FAKEGATEWAY_H

/*
 * An internet gateway that only exists for a test, on a loopback port the kernel
 * picks, and a QuickDC::PortForward driven against it.
 *
 * Shared rather than kept beside the port forwarding cases because two things need
 * it. Those cases ask what PortForward makes of a gateway that does not simply
 * agree; a hub session needs one to be *forwarded at all*, since a port the
 * gateway moved is the only way the port a client advertises differs from the port
 * it configured - and that difference is what a connect request naming the wrong
 * one looks like.
 *
 * Everything here is per test: no state is shared between the files that include
 * it, and nothing keeps a function-local static, so each translation unit having
 * its own copy changes nothing. The one thing this header does not provide is a
 * pump() - each including file already has one, and they differ.
 */

#include "quickdc.h"
#include "network/portforward.h"

#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/url.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/*
 * inline rather than static: a static function a translation unit does not happen
 * to call is a warning, and only one of the files including this uses faults. It
 * is safe here because nothing below keeps a function-local static, so the single
 * shared instance an inline function gets has no state to share.
 */


/* ------------------------------------------------------------------------- */

/* An IGD version 1 device: no AddAnyPortMapping, so a conflict is walked. */
inline constexpr const char* IGD1 =
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
inline constexpr const char* IGD2 =
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
inline constexpr const char* NOT_A_ROUTER =
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

inline std::string soapBody(const std::string& action, const std::string& inner)
{
	return "<?xml version=\"1.0\"?>"
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
		"<u:" + action + "Response"
		" xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
		+ inner +
		"</u:" + action + "Response></s:Body></s:Envelope>";
}

inline std::string soapFault(int code)
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

inline Answer ok(const std::string& body)
{
	Answer answer;
	answer.body = body;
	return answer;
}

inline Answer fault(int code)
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
inline void agreeable(FakeGateway& gateway, uint16_t reserved = 0)
{
	if (reserved)
		gateway.always("AddAnyPortMapping",
			ok(soapBody("AddAnyPortMapping",
				"<NewReservedPort>" + std::to_string(reserved) + "</NewReservedPort>")));

	gateway.always("GetExternalIPAddress",
		ok(soapBody("GetExternalIPAddress",
			"<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>")));
}

#endif // HAVE_QUICKDC_TEST_FAKEGATEWAY_H
