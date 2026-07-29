/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Http::Parser turns bytes off a socket into entities. It runs in both
 * directions - responses from the server a hub list is fetched from, and
 * requests from whoever connects to the built in HTTP server - so both are
 * somebody else's input, arriving a few bytes at a time.
 *
 * The awkward part of a parser fed like that is that every case has to work
 * whether the message arrives whole, in two pieces, or one byte per call, so
 * most of what follows is run both ways.
 */

#include "quickdc.h"
#include "network/http/httpparser.h"
#include "network/http/entity.h"
#include "network/http/header.h"
#include "network/http/pipeline.h"

#include <samurai/io/buffer.h>

#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/resource.h>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/*
 * One parser, its pipeline, and whatever it has produced. Entities come off the
 * pipeline owned by us, so they are kept here until the case is over.
 */
class Feed
{
	public:
		explicit Feed(bool request) : parser(request), last(Http::Parser::StatusMore) { }

		~Feed()
		{
			for (size_t n = 0; n < entities.size(); n++) delete entities[n];
		}

		Feed(const Feed&) = delete;
		Feed& operator=(const Feed&) = delete;

		/* Hands over the whole string in one call. */
		Http::Parser::Status put(const std::string& data)
		{
			Samurai::IO::Buffer in;
			in.append(data.data(), data.size());
			last = parser.addEntity(&in, &pipeline);
			collect();
			return last;
		}

		/* And the same, a byte at a time, as a slow connection delivers it. */
		Http::Parser::Status dribble(const std::string& data)
		{
			for (size_t n = 0; n < data.size(); n++)
			{
				Samurai::IO::Buffer in;
				in.append(&data[n], 1);
				last = parser.addEntity(&in, &pipeline);
				collect();
				if (last == Http::Parser::StatusError) return last;
			}
			return last;
		}

		size_t count() const { return entities.size(); }
		Http::Entity* at(size_t n) { return n < entities.size() ? entities[n] : 0; }
		Http::Entity* first() { return at(0); }

		Http::Parser parser;
		Http::Pipeline pipeline;
		Http::Parser::Status last;

	private:
		void collect()
		{
			while (Http::Entity* e = pipeline.pop()) entities.push_back(e);
		}

		std::vector<Http::Entity*> entities;
};

static std::string headerValue(Http::Entity* entity, const char* name)
{
	if (!entity) return std::string("(no entity)");
	Http::Header* header = entity->getHeader(name);
	if (!header || !header->getValue()) return std::string("(absent)");
	return std::string(header->getValue());
}

/* Peak resident bytes, for the one case that cannot be seen any other way. */
static size_t peakBytes()
{
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#ifdef __APPLE__
	return (size_t) usage.ru_maxrss;
#else
	return (size_t) usage.ru_maxrss * 1024;
#endif
}

int main()
{
	setbuf(stdout, 0);

	/* A response with headers and nothing else. */
	{
		Feed f(false);
		CHECK(f.put("HTTP/1.1 200 OK\r\nServer: test\r\nX-Empty:\r\n\r\n")
			== Http::Parser::StatusDone);
		CHECK(f.count() == 1);

		Http::Entity* e = f.first();
		CHECK(e && e->getCode() == 200);
		CHECK(e && e->getMessage() && std::string(e->getMessage()) == "OK");
		CHECK(e && e->getProtocol() == Http::Entity::HTTP_1_1);
		CHECK(e && !e->isRequest());
		CHECK(e && e->isCRLF());
		CHECK(headerValue(e, "Server") == "test");
		printf("  ok a response with no body parses\n");
	}

	/* The same response one byte at a time. */
	{
		Feed f(false);
		CHECK(f.dribble("HTTP/1.1 404 Not Found\r\nServer: test\r\n\r\n")
			== Http::Parser::StatusDone);
		CHECK(f.count() == 1);

		Http::Entity* e = f.first();
		CHECK(e && e->getCode() == 404);
		CHECK(e && std::string(e->getMessage()) == "Not Found");
		CHECK(headerValue(e, "Server") == "test");
		printf("  ok a response arriving one byte at a time parses the same\n");
	}

	/* Bare newlines, as older servers and hand typed requests use. */
	{
		Feed f(false);
		CHECK(f.put("HTTP/1.0 301 Moved\nLocation: http://elsewhere/\n\n")
			== Http::Parser::StatusDone);

		Http::Entity* e = f.first();
		CHECK(e && e->getCode() == 301);
		CHECK(e && e->getProtocol() == Http::Entity::HTTP_1_0);
		CHECK(e && !e->isCRLF());
		CHECK(headerValue(e, "Location") == "http://elsewhere/");
		printf("  ok a response using bare newlines parses\n");
	}

	/* A header name is matched whatever case it was sent in. */
	{
		Feed f(false);
		CHECK(f.put("HTTP/1.1 200 OK\r\nCoNtEnT-tYpE: text/html\r\n\r\n")
			== Http::Parser::StatusDone);

		Http::Entity* e = f.first();
		CHECK(headerValue(e, "Content-Type") == "text/html");
		CHECK(headerValue(e, "content-type") == "text/html");
		CHECK(e && e->countHeader("Content-Type") == 1);
		printf("  ok a header is found whatever case it came in\n");
	}

	/* The space after the colon is not part of the value, and is optional. */
	{
		Feed f(false);
		CHECK(f.put("HTTP/1.1 200 OK\r\nA:   spaced\r\nB:tight\r\n\r\n")
			== Http::Parser::StatusDone);

		Http::Entity* e = f.first();
		CHECK(headerValue(e, "A") == "spaced");
		CHECK(headerValue(e, "B") == "tight");
		printf("  ok the space after a colon is not part of the value\n");
	}

	/* A counted body is taken off the stream before the next message. */
	{
		Feed f(false);
		CHECK(f.put("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello")
			== Http::Parser::StatusDone);
		CHECK(f.count() == 1);

		/* Nothing of the body is left to be read as a header of the next. */
		CHECK(f.put("HTTP/1.1 204 No Content\r\n\r\n") == Http::Parser::StatusDone);
		CHECK(f.count() == 2);
		CHECK(f.at(1) && f.at(1)->getCode() == 204);
		printf("  ok a counted body is consumed, not left on the stream\n");
	}

	/* And is waited for when it has not all arrived. */
	{
		Feed f(false);
		CHECK(f.put("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhalf")
			== Http::Parser::StatusMore);
		CHECK(f.count() == 0);

		CHECK(f.put("way!") == Http::Parser::StatusMore);
		CHECK(f.count() == 0);

		CHECK(f.put("??") == Http::Parser::StatusDone);
		CHECK(f.count() == 1);
		printf("  ok a body split across reads is waited for\n");
	}

	/* A body of nothing is still a complete message. */
	{
		Feed f(false);
		CHECK(f.put("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
			== Http::Parser::StatusDone);
		CHECK(f.count() == 1);
		printf("  ok a zero length body completes the message\n");
	}

	/*
	 * An identity transfer encoding is the absence of one, so a message that
	 * says so and gives no length has no body rather than an unreadable one.
	 * The test for it compared the header's name, which is never "identity".
	 */
	{
		Feed f(false);
		CHECK(f.put("HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\n\r\n")
			== Http::Parser::StatusDone);
		CHECK(f.count() == 1);
		printf("  ok an identity transfer encoding is not a body we cannot read\n");
	}

	/* A chunked one is, and is refused while the framing is unimplemented. */
	{
		Feed f(false);
		CHECK(f.put("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n")
			== Http::Parser::StatusError);
		CHECK(f.count() == 0);
		printf("  ok a chunked body is refused rather than misread\n");
	}

	/*
	 * A status line with an empty reason phrase. The separator is required and
	 * the phrase is not, and proxies in front of HTTP/2 servers send exactly
	 * this - it was refused, which drops the response and the connection.
	 */
	{
		Feed f(false);
		CHECK(f.put("HTTP/1.1 200 \r\nServer: test\r\n\r\n") == Http::Parser::StatusDone);
		CHECK(f.count() == 1);

		Http::Entity* e = f.first();
		CHECK(e && e->getCode() == 200);
		CHECK(e && e->getMessage() && !*e->getMessage());
		printf("  ok a status line with an empty reason phrase parses\n");
	}

	/* A status line that is not one. */
	{
		const char* bad[] = {
			"NOT A STATUS LINE\r\n\r\n",
			"HTTP/1.1\r\n\r\n",
			"HTTP/1.1 \r\n\r\n",
			"HTTP/1.1 abc Reason\r\n\r\n",
			"garbage\r\n\r\n",
			0
		};

		for (size_t n = 0; bad[n]; n++)
		{
			Feed f(false);
			const Http::Parser::Status got = f.put(bad[n]);
			if (got != Http::Parser::StatusError)
			{
				printf("FAIL %s:%d accepted \"%s\"\n", __FILE__, __LINE__, bad[n]);
				failures++;
			}
			CHECK(f.count() == 0);
		}
		printf("  ok a status line that is not one is refused\n");
	}

	/* Nothing to parse is not an error; the read simply came up empty. */
	{
		Feed f(false);
		CHECK(f.put("") == Http::Parser::StatusMore);
		CHECK(f.count() == 0);

		/* And the parser is still usable afterwards. */
		CHECK(f.put("HTTP/1.1 200 OK\r\n\r\n") == Http::Parser::StatusDone);
		CHECK(f.count() == 1);
		printf("  ok an empty read is not an error\n");
	}

	/* A header line nobody is going to finish. */
	{
		Feed f(false);
		CHECK(f.put("HTTP/1.1 200 OK\r\n") == Http::Parser::StatusMore);

		const std::string huge(20000, 'x');
		CHECK(f.put("X-Long: " + huge) == Http::Parser::StatusError);
		printf("  ok a header line that never ends is refused\n");
	}

	/* A request, which is the other direction the same parser runs in. */
	{
		Feed f(true);
		CHECK(f.put("GET /hublist.xml.bz2 HTTP/1.1\r\nHost: example\r\n\r\n")
			== Http::Parser::StatusDone);

		Http::Entity* e = f.first();
		CHECK(e && e->isRequest());
		CHECK(e && e->getMethod() == Http::Entity::GET);
		CHECK(e && e->getURI() && std::string(e->getURI()) == "/hublist.xml.bz2");
		CHECK(e && e->getProtocol() == Http::Entity::HTTP_1_1);
		CHECK(headerValue(e, "Host") == "example");
		printf("  ok a request line parses\n");
	}

	/* The method is matched whatever case it came in, and unknown ones say so. */
	{
		Feed f(true);
		CHECK(f.put("head /index.html HTTP/1.0\r\n\r\n") == Http::Parser::StatusDone);
		CHECK(f.first() && f.first()->getMethod() == Http::Entity::HEAD);
		CHECK(f.first() && f.first()->getProtocol() == Http::Entity::HTTP_1_0);

		Feed g(true);
		CHECK(g.put("BREW /coffee HTTP/1.1\r\n\r\n") == Http::Parser::StatusDone);
		CHECK(g.first() && g.first()->getMethod() == Http::Entity::UNKNOWN);
		printf("  ok the method is recognised, or reported as unknown\n");
	}

	/* A request with a body. */
	{
		Feed f(true);
		CHECK(f.put("POST /submit HTTP/1.1\r\nContent-Length: 4\r\n\r\nbody")
			== Http::Parser::StatusDone);
		CHECK(f.first() && f.first()->getMethod() == Http::Entity::POST);
		printf("  ok a request with a counted body parses\n");
	}

	/* A request line that is not one. */
	{
		const char* bad[] = {
			"GET\r\n\r\n",
			"GET /\r\n\r\n",
			"GET  \r\n\r\n",
			"nonsense\r\n\r\n",
			0
		};

		for (size_t n = 0; bad[n]; n++)
		{
			Feed f(true);
			if (f.put(bad[n]) != Http::Parser::StatusError)
			{
				printf("FAIL %s:%d accepted \"%s\"\n", __FILE__, __LINE__, bad[n]);
				failures++;
			}
		}
		printf("  ok a request line that is not one is refused\n");
	}

	/* Header lines on their own. */
	{
		Http::Header ok("Content-Type: text/plain");
		CHECK(ok.isValid());
		CHECK(std::string(ok.getName()) == "Content-Type");
		CHECK(std::string(ok.getValue()) == "text/plain");

		Http::Header nocolon("Content-Type text/plain");
		CHECK(!nocolon.isValid());

		Http::Header empty("");
		CHECK(!empty.isValid());

		Http::Header spaces("X:      value");
		CHECK(spaces.isValid());
		CHECK(std::string(spaces.getValue()) == "value");

		/* Only spaces after the colon leaves the value empty, not adrift. */
		Http::Header blank("X:   ");
		CHECK(blank.isValid());
		CHECK(blank.getValue() && !*blank.getValue());
		printf("  ok a header line splits at the first colon\n");
	}

	/*
	 * Both line parsers duplicate the line to cut it up, and neither freed it,
	 * so every message parsed leaked its own first line. Nothing else here can
	 * see that, so this measures it: twenty thousand messages with an eight
	 * kilobyte reason phrase is a hundred and sixty megabytes of leak.
	 */
	{
		const size_t before = peakBytes();
		CHECK(before > 0);

		const std::string reason(8192, 'R');
		for (int n = 0; n < 20000; n++)
		{
			Feed f(false);
			if (f.put("HTTP/1.1 200 " + reason + "\r\n\r\n") != Http::Parser::StatusDone)
			{
				printf("FAIL %s:%d round %d did not parse\n", __FILE__, __LINE__, n);
				failures++;
				break;
			}
		}

		const size_t after = peakBytes();
		const size_t grew = (after > before) ? (after - before) : 0;

		CHECK(grew < 50u * 1024u * 1024u);
		printf("  ok parsing does not leak the line it parsed (grew %zu KB)\n",
			grew / 1024);
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
