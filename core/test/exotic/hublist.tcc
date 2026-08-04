/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * QuickDC::HubList against an HTTP server that only exists for the test, on a
 * loopback port the kernel picks.
 *
 * A case here is a response, or a sequence of them, and the outcome it should
 * produce. A sequence rather than a single response because the HTTP client sends
 * Connection: close and so a followed redirect is a second connection: the server
 * hands out one canned answer per connection, and the last one repeats for as
 * long as anything keeps asking.
 *
 * The interesting responses are the hostile ones - a bzip2 bomb, a stream that
 * stops halfway, XML that never closes its tags. Every one of those has to end as
 * a refusal and not as a shorter hublist, because a list missing hubs looks
 * exactly like one that never had them.
 */

#include "quickdc.h"
#include "network/hublist.h"

#include <samurai/io/compression.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/net/url.h>

#include <chrono>
#include <memory>
#include <span>
#include <stdio.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

/* Generous: one case compresses seventeen megabytes, and a sanitized build is
   slower at everything around it. */
static const int TIMEOUT_MS = 30000;

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

/*
 * The other end: a listening socket, and one canned answer per connection.
 *
 * Connections are served one at a time, which is all the client ever opens, and
 * 'answers' is indexed by how many have been served with the last entry repeating
 * - so a server that redirects forever is one answer rather than six.
 */
class FakeServer :
	public Samurai::IO::Net::ServerSocketEventHandler,
	public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::vector<std::string> answers;
		std::vector<std::string> requests;

		/*
		 * Answer without waiting for a request, which is how a server that is
		 * not going to complete a TLS handshake behaves: the ClientHello is not
		 * a header block, so there is no end of one to wait for.
		 */
		bool talkFirst = false;

		bool listen()
		{
			Samurai::IO::Net::InetSocketAddress any(static_cast<uint16_t>(0));
			server = Samurai::IO::Net::ServerSocket::create(this, any);
			return server && server->getFD() != -1 && server->listen();
		}

		uint16_t port() const { return server ? server->getLocalPort() : 0; }
		size_t served() const { return requests.size(); }

		/*
		 * Read what the client sent, write what we owe it, and close.
		 *
		 * Driven from the pump rather than from EventCanWrite alone: a couple of
		 * megabytes over loopback goes out a socket buffer at a time, and the
		 * write notifier is not the only thing that makes room in one.
		 */
		void step()
		{
			if (!peer) return;

			drain();

			if (!armed)
			{
				/* Nothing is answered before the request is whole, so a case
				   cannot pass by accident on a server that talked first. */
				if (!talkFirst && request.find("\r\n\r\n") == std::string::npos) return;

				requests.push_back(request);
				outgoing = answers.empty() ? std::string()
					: answers[requests.size() <= answers.size()
						? requests.size() - 1 : answers.size() - 1];
				sent = 0;
				armed = true;
			}

			while (sent < outgoing.size())
			{
				const ssize_t got = peer->write(outgoing.data() + sent, outgoing.size() - sent);
				if (got <= 0) break;
				sent += static_cast<size_t>(got);
			}

			/* The client asked for Connection: close, and a response with no
			   Content-Length is delimited by exactly this. */
			if (sent == outgoing.size())
			{
				peer.reset();
				request.clear();
				armed = false;
			}
		}

		void drain()
		{
			if (!peer) return;
			for (;;)
			{
				char buf[4096];
				size_t got = 0;
				std::error_code ec;
				if (peer->read(buf, sizeof(buf), got, ec) != Samurai::IO::ReadResult::Ok) break;
				if (!got) break;
				request.append(buf, got);
			}
		}

		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override { }

		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket> socket) override
		{
			peer = socket;
			peer->setEventHandler(this);
			request.clear();
			armed = false;
		}

		void EventHostLookup(const Samurai::IO::Net::Socket*) override { }
		void EventHostFound(const Samurai::IO::Net::Socket*) override { }
		void EventConnecting(const Samurai::IO::Net::Socket*) override { }
		void EventConnected(const Samurai::IO::Net::Socket*) override { }
		void EventTimeout(const Samurai::IO::Net::Socket*) override { }
		void EventDisconnected(const Samurai::IO::Net::Socket*) override { }
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { }
		void EventCanWrite(const Samurai::IO::Net::Socket*) override { }
		void EventTLSConnected(const Samurai::IO::Net::Socket*) override { }
		void EventTLSDisconnected(const Samurai::IO::Net::Socket*) override { }
		void EventError(const Samurai::IO::Net::Socket*,
			Samurai::IO::Net::SocketError, const char*) override { }

	private:
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		std::shared_ptr<Samurai::IO::Net::Socket> peer;
		std::string request;
		std::string outgoing;
		size_t sent = 0;
		bool armed = false;
};

/* What the downloader told whoever asked for the list. */
class Listener : public QuickDC::HubListListener
{
	public:
		std::vector<QuickDC::HubListEntry> hubs;
		std::string failure;
		int answers = 0;

		void EventHubList(const std::vector<QuickDC::HubListEntry>& list) override
		{
			hubs = list;
			answers++;
		}

		void EventHubListFailure(const char* reason) override
		{
			failure = reason ? reason : "";
			answers++;
		}
};

/* A server with its answers queued, and one download run against it to the end. */
class Fetch
{
	public:
		FakeServer server;
		Listener listener;
		QuickDC::HubList hublist;
		bool answered = false;

		explicit Fetch(const std::string& raw, const char* scheme = "http",
		               bool talkFirst = false)
			: Fetch(std::vector<std::string>{ raw }, scheme, talkFirst) { }

		explicit Fetch(std::vector<std::string> answers, const char* scheme = "http",
		               bool talkFirst = false)
			: hublist(&listener)
		{
			if (!server.listen()) return;
			server.answers = std::move(answers);
			server.talkFirst = talkFirst;

			const std::string address = std::string(scheme) + "://127.0.0.1:"
				+ std::to_string(server.port()) + "/hublist.xml";

			hublist.download(Samurai::IO::Net::URL(address));

			answered = pump([&] { server.step(); return listener.answers != 0; });
		}

		Fetch(const Fetch&) = delete;
		Fetch& operator=(const Fetch&) = delete;

		bool ok() const
		{ return answered && listener.answers == 1 && listener.failure.empty(); }

		bool refused() const
		{ return answered && listener.answers == 1 && !listener.failure.empty(); }

		const std::string& why() const { return listener.failure; }
		const std::vector<QuickDC::HubListEntry>& hubs() const { return listener.hubs; }
};

/* ------------------------------------------------------------------------- */

static std::string reply(int code, const char* phrase, std::string_view body,
                         std::string_view extra = std::string_view())
{
	std::string out = "HTTP/1.1 ";
	out += std::to_string(code);
	out += " ";
	out += phrase;
	out += "\r\nContent-Type: application/octet-stream\r\nContent-Length: ";
	out += std::to_string(body.size());
	out += "\r\n";
	out.append(extra);
	out += "Connection: close\r\n\r\n";
	out.append(body);
	return out;
}

/* No Content-Length at all: the body runs to the close, which some servers do. */
static std::string replyUntilClose(std::string_view body)
{
	std::string out = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
	out.append(body);
	return out;
}

static std::string redirect(const char* where)
{
	std::string extra = "Location: ";
	extra += where;
	extra += "\r\n";
	return reply(302, "Found", "<html>moved</html>", extra);
}

static std::string compress(std::string_view plain)
{
	Samurai::IO::BZip2Compressor codec;

	std::string out;
	char window[65536];
	std::span<const char> in(plain.data(), plain.size());

	while (true)
	{
		const Samurai::IO::Codec::Progress p = codec.step(in, window);
		if (p.status == Samurai::IO::Codec::Status::Error) return std::string();

		out.append(window, p.produced);
		in = in.subspan(p.consumed);

		if (p.status == Samurai::IO::Codec::Status::StreamEnd) return out;

		/* Nothing moved and nothing left to give: a flush produces on every
		   call, so this would be the compressor stuck rather than finishing. */
		if (!p.consumed && !p.produced) return std::string();
	}
}

static const char* HUBLIST_XML =
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
	"<Hublist>\n"
	"  <Hubs>\n"
	"    <Hub Name=\"Some Hub\" Address=\"dchub://hub.example.org:411\""
	" Description=\"A hub\" Users=\"120\" Shared=\"4096000000\""
	" Country=\"Norway\" Minshare=\"0\" Minslots=\"0\" Maxhubs=\"0\" Maxusers=\"500\"/>\n"
	"    <Hub Name=\"Another\" Address=\"adc://adc.example.net:2780\""
	" Description=\"&amp; another\" Users=\"7\" Shared=\"1024\""
	" Country=\"Sweden\" Minshare=\"1048576\" Minslots=\"2\" Maxhubs=\"5\" Maxusers=\"50\"/>\n"
	"  </Hubs>\n"
	"</Hublist>\n";

/*
 * Every field of the document above, so a value read out of the wrong attribute
 * is as visible as one that went missing.
 *
 * Reports which field was wrong rather than only that something was, which is
 * why this is a function and not twenty assertions at each call site. Callers
 * wrap it in EXO_ASSERT.
 */
#define FIELD(expr) \
	do { \
		if (!(expr)) { \
			printf("    wrong field: %s\n", #expr); \
			return false; \
		} \
	} while (0)

static bool checkSample(const std::vector<QuickDC::HubListEntry>& hubs)
{
	FIELD(hubs.size() == 2);

	FIELD(hubs[0].name == "Some Hub");
	FIELD(hubs[0].address == "dchub://hub.example.org:411");
	FIELD(hubs[0].description == "A hub");
	FIELD(hubs[0].country == "Norway");
	FIELD(hubs[0].users == 120);
	FIELD(hubs[0].shared == 4096000000ULL);
	FIELD(hubs[0].minshare == 0);
	FIELD(hubs[0].minslots == 0);
	FIELD(hubs[0].maxhubs == 0);
	FIELD(hubs[0].maxusers == 500);

	FIELD(hubs[1].name == "Another");
	FIELD(hubs[1].address == "adc://adc.example.net:2780");
	FIELD(hubs[1].description == "& another");
	FIELD(hubs[1].country == "Sweden");
	FIELD(hubs[1].users == 7);
	FIELD(hubs[1].shared == 1024);
	FIELD(hubs[1].minshare == 1048576);
	FIELD(hubs[1].minslots == 2);
	FIELD(hubs[1].maxhubs == 5);
	FIELD(hubs[1].maxusers == 50);
	return true;
}

#undef FIELD

/* A list of 'target' bytes or so, to say where the size limits are. */
static std::string bulkHublist(size_t target, size_t& count)
{
	std::string out = "<Hublist><Hubs>";
	count = 0;

	while (out.size() < target)
	{
		const std::string n = std::to_string(count);
		out += "<Hub Name=\"hub" + n + "\" Address=\"dchub://hub" + n
			+ ".example.org:411\" Description=\"one of very many hubs\""
			  " Users=\"3\" Shared=\"4096\" Country=\"Norway\" Minshare=\"0\""
			  " Minslots=\"0\" Maxhubs=\"0\" Maxusers=\"0\"/>";
		count++;
	}

	out += "</Hubs></Hublist>";
	return out;
}

/* ------------------------------------------------------------------------- */

/* ---- what a hublist server is supposed to send ---- */
EXO_TEST(hublist_a_plain_xml_hublist_is_read_field_by_field,
{
		Fetch f(reply(200, "OK", HUBLIST_XML));
		EXO_ASSERT(f.ok());
		EXO_ASSERT(checkSample(f.hubs()));

		/* And it asked for the path the URL named, over HTTP/1.1. */
		EXO_ASSERT(f.server.served() == 1);
		EXO_ASSERT(f.server.requests.size() == 1
			&& f.server.requests[0].compare(0, 25, "GET /hublist.xml HTTP/1.1") == 0);
	
	return 1;
});

EXO_TEST(hublist_the_same_list_bzip2_compressed_reads_the_same_u_bytes_packed,
{
		const std::string packed = compress(HUBLIST_XML);
		EXO_ASSERT(!packed.empty());

		Fetch f(reply(200, "OK", packed));
		EXO_ASSERT(f.ok());
		EXO_ASSERT(checkSample(f.hubs()));
	
	return 1;
});

/* A server that delimits the body by closing rather than by a length. */
EXO_TEST(hublist_a_body_delimited_by_the_close_is_read,
{
		Fetch f(replyUntilClose(HUBLIST_XML));
		EXO_ASSERT(f.ok());
		EXO_ASSERT(checkSample(f.hubs()));
	
	return 1;
});

/* Element and attribute names are the server's, and generators disagree on
	   the case of every one of them. */
EXO_TEST(hublist_names_are_matched_without_regard_to_case,
{
		Fetch f(reply(200, "OK",
			"<HUBLIST><HUBS><hub name=\"x\" ADDRESS=\"dchub://x:411\""
			" users=\"5\" maxusers=\"9\"/></HUBS></HUBLIST>"));
		EXO_ASSERT(f.ok());
		EXO_ASSERT(f.hubs().size() == 1);
		if (f.hubs().size() == 1)
		{
			EXO_ASSERT(f.hubs()[0].name == "x");
			EXO_ASSERT(f.hubs()[0].address == "dchub://x:411");
			EXO_ASSERT(f.hubs()[0].users == 5);
			EXO_ASSERT(f.hubs()[0].maxusers == 9);
		}
	
	return 1;
});

/* A list of no hubs is an answer, not a failure. */
EXO_TEST(hublist_an_empty_hubs_is_an_empty_list_and_not_an_error,
{
		Fetch f(reply(200, "OK", "<Hublist><Hubs/></Hublist>"));
		EXO_ASSERT(f.ok());
		EXO_ASSERT(f.hubs().empty());
	
	return 1;
});

/*
	 * A hublist address that has not moved at least once is the exception, so
	 * the redirect is followed and only the list at the end of it is reported.
	 */
EXO_TEST(hublist_a_redirect_is_followed_to_the_list_at_the_end_of,
{
		Fetch f({ redirect("/moved/hublist.xml"), reply(200, "OK", HUBLIST_XML) });
		EXO_ASSERT(f.ok());
		EXO_ASSERT(checkSample(f.hubs()));
		EXO_ASSERT(f.server.served() == 2);
		EXO_ASSERT(f.server.requests.size() == 2
			&& f.server.requests[1].compare(0, 12, "GET /moved/h") == 0);
	
	return 1;
});

/* Several of them, up to the budget, still arrive somewhere. */
EXO_TEST(hublist_a_chain_up_to_the_limit_is_followed,
{
		Fetch f({ redirect("/one"), redirect("/two"), redirect("/three"),
			redirect("/four"), redirect("/five"), reply(200, "OK", HUBLIST_XML) });
		EXO_ASSERT(f.ok());
		EXO_ASSERT(checkSample(f.hubs()));
		EXO_ASSERT(f.server.served() == 6);
	
	return 1;
});

/*
	 * A server that redirects forever. The last canned answer repeats, so this
	 * is one redirect that keeps being served - and the budget is what stops it
	 * rather than the case running out of answers.
	 */
EXO_TEST(hublist_a_chain_longer_than_the_limit_is_refused,
{
		Fetch f(redirect("/round-and-round"));
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.why().find("redirected more than") != std::string::npos);
		EXO_ASSERT(f.hubs().empty());

		/* The first request plus one per redirect followed, and no more. */
		EXO_ASSERT(f.server.served() == QUICKDC_HUBLIST_MAX_REDIRECTS + 1);
	
	return 1;
});

/*
	 * A 3xx with no Location says the list moved without saying where. There is
	 * nothing to follow, so it arrives as a response - and reading its body as a
	 * hublist would report a list of no hubs, which is a legitimate answer.
	 */
EXO_TEST(hublist_a_redirect_naming_nowhere_is_reported_as_one,
{
		Fetch f(reply(302, "Found", "<html>moved, somewhere</html>"));
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.why().find("redirected") != std::string::npos);
		EXO_ASSERT(f.hubs().empty());
		EXO_ASSERT(f.server.served() == 1);
	
	return 1;
});

/* ---- what else a URL can answer with ---- */
EXO_TEST(hublist_a_404_is_reported_with_its_status,
{
		Fetch f(reply(404, "Not Found", "<html>no such list</html>"));
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.why().find("404") != std::string::npos);
		EXO_ASSERT(f.why().find("Not Found") != std::string::npos);
		EXO_ASSERT(f.hubs().empty());
	
	return 1;
});

EXO_TEST(hublist_a_500_is_reported_with_its_status,
{
		Fetch f(reply(500, "Internal Server Error", ""));
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.why().find("500") != std::string::npos);
	
	return 1;
});

/* Well formed XML that is not a hublist, which is what a captive portal or
	   an error page served with a 200 looks like. */
EXO_TEST(hublist_a_document_that_is_not_a_hublist_is_refused,
{
		Fetch f(reply(200, "OK", "<html><body><p>Sign in to continue</p></body></html>"));
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.why().find("not a hublist") != std::string::npos);
		EXO_ASSERT(f.hubs().empty());
	
	return 1;
});

EXO_TEST(hublist_an_empty_body_is_refused,
{
		Fetch f(reply(200, "OK", ""));
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.hubs().empty());
	
	return 1;
});

/*
	 * An https URL is negotiated rather than refused: the request goes out over
	 * TLS or not at all. This server answers the ClientHello with bytes that are
	 * not a TLS record, which is the shape of everything that can go wrong with
	 * the far end - and the answer has to name TLS, because "could not connect"
	 * would send whoever pasted the address looking at their network.
	 */
EXO_TEST(hublist_an_https_server_that_fails_the_handshake_names_tls,
{
		std::string junk(512, 'x');
		junk += "\r\n\r\n";

		Fetch f(junk, "https", true);
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.why().find("certificate could not be verified") != std::string::npos);
		EXO_ASSERT(f.hubs().empty());

		/* Nothing was sent in the clear: the server saw a handshake and no
		   request line at all. */
		EXO_ASSERT(f.server.requests.size() == 1
			&& f.server.requests[0].find("GET ") == std::string::npos);
	
	return 1;
});

/*
	 * A list larger than we are willing to read. The Content-Length alone is
	 * enough to refuse it, so nothing is transferred: a server that says it is
	 * about to send a gigabyte is not made more trustworthy by receiving it.
	 */
EXO_TEST(hublist_a_body_over_the_download_limit_is_refused,
{
		std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: ";
		raw += std::to_string(QUICKDC_HUBLIST_MAX_DOWNLOAD + 1);
		raw += "\r\nConnection: close\r\n\r\n";

		Fetch f(raw);
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.why().find("larger than") != std::string::npos);
		EXO_ASSERT(f.hubs().empty());
	
	return 1;
});

/*
	 * And a real one, which is larger than the megabyte the HTTP client allows
	 * by default: the limit has to have been raised deliberately for this to
	 * arrive at all.
	 */
EXO_TEST(hublist_a_u_byte_list_of_u_hubs_is_under_the_raised,
{
		size_t count = 0;
		const std::string body = bulkHublist(2 * 1024 * 1024, count);
		EXO_ASSERT(body.size() > 1024 * 1024);

		Fetch f(reply(200, "OK", body));
		EXO_ASSERT(f.ok());
		EXO_ASSERT(f.hubs().size() == count);
	
	return 1;
});

/*
	 * A bzip2 bomb: a few kilobytes that unpack to more than the plain limit.
	 * Refused while it is being unpacked, so the ceiling is the limit and not
	 * whatever the stream would have reached, and nothing partial is handed on.
	 */
EXO_TEST(hublist_a_u_byte_bzip2_bomb_is_refused,
{
		const std::string bomb =
			compress(std::string(QUICKDC_HUBLIST_MAX_PLAIN + 1024 * 1024, '\0'));

		EXO_ASSERT(!bomb.empty());
		EXO_ASSERT(bomb.size() < QUICKDC_HUBLIST_MAX_DOWNLOAD);

		Fetch f(reply(200, "OK", bomb));
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.why().find("unpacks to more") != std::string::npos);
		EXO_ASSERT(f.hubs().empty());
	
	return 1;
});

/*
	 * A stream that stops in the middle. Whatever came out of it before it did
	 * is thrown away rather than parsed: a hublist cut off after its first few
	 * hubs is well formed XML as far as the tags go.
	 */
EXO_TEST(hublist_a_truncated_bzip2_stream_is_refused,
{
		size_t count = 0;
		const std::string packed = compress(bulkHublist(256 * 1024, count));
		EXO_ASSERT(packed.size() > 64);

		Fetch f(reply(200, "OK", packed.substr(0, packed.size() / 2)));
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.hubs().empty());
	
	return 1;
});

/* Something that claims to be bzip2 and is not. */
EXO_TEST(hublist_bytes_that_only_start_like_bzip2_are_refused,
{
		Fetch f(reply(200, "OK", std::string("BZh9") + std::string(512, 'x')));
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.hubs().empty());
	
	return 1;
});

/*
	 * Tags that never close. The reader reports this and carries on, so the
	 * entries after it describe a document nobody wrote - and the ones before it
	 * are a hublist that is missing however many the rest of the file held.
	 */
EXO_TEST(hublist_xml_that_does_not_close_its_tags_is_refused,
{
		Fetch f(reply(200, "OK",
			"<Hublist><Hubs>"
			"<Hub Name=\"a\" Address=\"dchub://a:411\"/>"
			"<Hub Name=\"b\" Address=\"dchub://b:411\"/>"));
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.why().find("XML") != std::string::npos);
		EXO_ASSERT(f.hubs().empty());
	
	return 1;
});

EXO_TEST(hublist_end_tags_with_no_start_tags_are_refused,
{
		Fetch f(reply(200, "OK", "</Hublist></Hubs>"));
		EXO_ASSERT(f.refused());
		EXO_ASSERT(f.hubs().empty());
	
	return 1;
});

/*
	 * An entry with no Address names nowhere to connect to. It is dropped and
	 * the rest of the list is kept: one unusable row is a generator's bug, and
	 * refusing everything over it would cost a user every other hub on the list.
	 */
EXO_TEST(hublist_an_entry_with_no_address_is_dropped_and_the_list_is,
{
		Fetch f(reply(200, "OK",
			"<Hublist><Hubs>"
			"<Hub Name=\"first\" Address=\"dchub://first:411\" Users=\"1\"/>"
			"<Hub Name=\"nowhere\" Description=\"no address at all\" Users=\"99\"/>"
			"<Hub Name=\"empty\" Address=\"\" Users=\"98\"/>"
			"<Hub Name=\"last\" Address=\"dchub://last:411\" Users=\"2\"/>"
			"</Hubs></Hublist>"));

		EXO_ASSERT(f.ok());
		EXO_ASSERT(f.hubs().size() == 2);
		if (f.hubs().size() == 2)
		{
			EXO_ASSERT(f.hubs()[0].name == "first");
			EXO_ASSERT(f.hubs()[1].name == "last");
		}
	
	return 1;
});

/* Every other field absent, which is legal: only the address is required. */
EXO_TEST(hublist_an_address_on_its_own_is_a_usable_entry,
{
		Fetch f(reply(200, "OK",
			"<Hublist><Hubs><Hub Address=\"dchub://bare:411\"/></Hubs></Hublist>"));

		EXO_ASSERT(f.ok());
		EXO_ASSERT(f.hubs().size() == 1);
		if (f.hubs().size() == 1)
		{
			EXO_ASSERT(f.hubs()[0].address == "dchub://bare:411");
			EXO_ASSERT(f.hubs()[0].name.empty());
			EXO_ASSERT(f.hubs()[0].users == 0);
			EXO_ASSERT(f.hubs()[0].shared == 0);
		}
	
	return 1;
});

/* Numbers the server made up. These are what the list claims, and saturating
	   on one that does not fit is the parser's business, not this file's. */
EXO_TEST(hublist_numbers_that_are_not_numbers_do_not_refuse_the_list,
{
		Fetch f(reply(200, "OK",
			"<Hublist><Hubs><Hub Address=\"dchub://n:411\" Users=\"-1\""
			" Shared=\"99999999999999999999999999\" Maxusers=\"twelve\"/></Hubs></Hublist>"));

		EXO_ASSERT(f.ok());
		EXO_ASSERT(f.hubs().size() == 1);
	
	return 1;
});

/* ---- the socket does not outlive the download ---- */
EXO_TEST(hublist_repeated_downloads_leave_nothing_behind,
{
		Samurai::IO::Net::SocketMonitor* monitor =
			Samurai::IO::Net::SocketMonitor::getInstance();
		const size_t baseline = monitor->size();

		for (int n = 0; n < 3; n++)
		{
			Fetch f(reply(200, "OK", HUBLIST_XML));
			EXO_ASSERT(f.ok());
		}

		pump([&] { return monitor->size() <= baseline; }, 2000);
		EXO_ASSERT(monitor->size() == baseline);
	
	return 1;
});

/* An abandoned download says nothing at all. */
EXO_TEST(hublist_a_cancelled_download_reports_nothing,
{
		FakeServer server;
		EXO_ASSERT(server.listen());

		Listener listener;
		QuickDC::HubList hublist(&listener);

		const std::string address =
			"http://127.0.0.1:" + std::to_string(server.port()) + "/hublist.xml";

		hublist.download(Samurai::IO::Net::URL(address));
		hublist.cancel();

		pump([] { return false; }, 200);

		EXO_ASSERT(listener.answers == 0);
		EXO_ASSERT(!hublist.isBusy());
	
	return 1;
});
