/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Anything that connects to us is fingerprinted before it is spoken to, and
 * what the fingerprint says decides which session it is handed to. The bytes
 * are the first thing an unauthenticated peer sends, so guessing wrong hands a
 * stranger to the wrong protocol handler.
 *
 * The dispatch itself needs a socket and a core; what is checked here is the
 * telling apart, which is pure.
 */

#include "quickdc.h"
#include "network/probe.h"

#include <stdio.h>
#include <string.h>
#include <string>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

using Protocol = QuickDC::Protocol;

static Protocol identify(const std::string& data)
{
	return QuickDC::identify(data.data(), data.size());
}

static const char* name(Protocol p)
{
	switch (p)
	{
		case Protocol::TLS:     return "TLS";
		case Protocol::ADC:     return "ADC";
		case Protocol::ADCHub:  return "ADCHub";
		case Protocol::DC:      return "DC";
		case Protocol::HTTP:    return "HTTP";
		case Protocol::Unknown: return "Unknown";
	}
	return "?";
}

static void expect(const std::string& data, Protocol want, const char* what)
{
	const Protocol got = identify(data);
	if (got != want)
	{
		printf("FAIL %s:%d %s: expected %s, got %s\n",
			__FILE__, __LINE__, what, name(want), name(got));
		failures++;
	}
}

/* A TLS 1.x ClientHello record, as far as the probe looks into it. */
static std::string clientHello(char recordMajor, char recordMinor, char helloMajor)
{
	std::string out;
	out += (char) 22;            /* handshake record */
	out += recordMajor;
	out += recordMinor;
	out += (char) 0x01;          /* record length, high */
	out += (char) 0x00;          /* record length, low */
	out += (char) 1;             /* ClientHello */
	out += (char) 0x00;          /* handshake length */
	out += (char) 0x00;
	out += (char) 0xfc;
	out += helloMajor;           /* the client's own version */
	out += (char) 0x03;
	while (out.size() < 16) out += (char) 0x5a;
	return out;
}

int main()
{
	setbuf(stdout, 0);

	/* The two ADC greetings, which differ only in who they are addressed to. */
	{
		expect("CSUP ADBASE ADTIGR\n", Protocol::ADC, "a client's SUP");
		expect("HSUP ADBASE ADTIGR\n", Protocol::ADCHub, "a hub's SUP");
		expect("CSUP", Protocol::ADC, "a bare CSUP");
		printf("  ok the two ADC greetings are told apart\n");
	}

	/* NMDC announces itself with a nick. */
	{
		expect("$MyNick bob|", Protocol::DC, "a nick");
		expect("$MyNick ", Protocol::DC, "the shortest nick line");
		printf("  ok an NMDC greeting is recognised\n");
	}

	/* The methods the HTTP server answers. */
	{
		expect("GET / HTTP/1.1\r\n", Protocol::HTTP, "GET");
		expect("HEAD / HTTP/1.1\r\n", Protocol::HTTP, "HEAD");
		expect("POST / HTTP/1.1\r\n", Protocol::HTTP, "POST");
		expect("PUT / HTTP/1.1\r\n", Protocol::HTTP, "PUT");
		expect("OPTIONS * HTTP/1.1\r\n", Protocol::HTTP, "OPTIONS");
		expect("TRACE / HTTP/1.1\r\n", Protocol::HTTP, "TRACE");
		expect("DELETE / HTTP/1.1\r\n", Protocol::HTTP, "DELETE");
		printf("  ok an HTTP request is recognised\n");
	}

	/* And a method it does not, which is not a reason to guess. */
	{
		expect("PATCH / HTTP/1.1\r\n", Protocol::Unknown, "PATCH");
		expect("BREW /coffee HTTP/1.1\r\n", Protocol::Unknown, "BREW");
		printf("  ok a method we do not answer is not taken for HTTP\n");
	}

	/* A TLS handshake, which has to be finished before anything can be read. */
	{
		expect(clientHello(3, 1, 3), Protocol::TLS, "a TLS 1.0 record");
		expect(clientHello(3, 3, 3), Protocol::TLS, "a TLS 1.2 record");

		/*
		 * A TLS 1.3 client sends a 1.0 record carrying a 1.2 client version for
		 * middlebox compatibility, so the two are not required to agree.
		 */
		expect(clientHello(3, 1, 3), Protocol::TLS, "a TLS 1.3 record");
		printf("  ok a TLS handshake is recognised\n");
	}

	/* Something that is not one. */
	{
		/* A record that is not a handshake. */
		std::string appdata = clientHello(3, 1, 3);
		appdata[0] = (char) 23;
		expect(appdata, Protocol::Unknown, "an application data record");

		/* A handshake that is not a ClientHello. */
		std::string server = clientHello(3, 1, 3);
		server[5] = (char) 2;
		expect(server, Protocol::Unknown, "a ServerHello");

		/* Something older than SSL 3.0. */
		std::string ancient = clientHello(2, 0, 3);
		expect(ancient, Protocol::Unknown, "an SSL 2.0 record version");
		printf("  ok a record that is not a client handshake is refused\n");
	}

	/*
	 * The high bit set in the first byte is the old SSLv2 handshake, and it is
	 * also where a signed char stops behaving: every fourcc here is built from
	 * one, so a byte over 127 used to spread through the whole word.
	 */
	{
		std::string v2;
		v2 += (char) 0x80;   /* length, high bit set */
		v2 += (char) 0x2e;
		v2 += (char) 1;      /* ClientHello */
		v2 += (char) 3;      /* version major */
		v2 += (char) 1;
		while (v2.size() < 16) v2 += (char) 0x5a;
		expect(v2, Protocol::TLS, "an SSLv2 hello");
		printf("  ok an SSLv2 handshake is recognised\n");
	}

	/* Bytes over 127 anywhere in the fourcc are not any of ours. */
	{
		const char high[] = { (char) 0xc3, 'S', 'U', 'P', ' ', 'A', 'D', 'B', 'A', 'S', 'E' };
		expect(std::string(high, sizeof(high)), Protocol::Unknown, "a high byte first");

		const char high2[] = { 'C', (char) 0xc3, 'U', 'P', ' ', 'A', 'D', 'B', 'A', 'S', 'E' };
		expect(std::string(high2, sizeof(high2)), Protocol::Unknown, "a high byte inside");

		const char allhigh[] = { (char) 0xff, (char) 0xff, (char) 0xff, (char) 0xff,
			(char) 0xff, (char) 0xff, (char) 0xff, (char) 0xff };
		expect(std::string(allhigh, sizeof(allhigh)), Protocol::Unknown, "all high bytes");
		printf("  ok a byte over 127 does not spread through the fourcc\n");
	}

	/* Too little to tell yet is not a protocol. */
	{
		expect("", Protocol::Unknown, "nothing");
		expect("C", Protocol::Unknown, "one byte");
		expect("CSU", Protocol::Unknown, "three bytes of CSUP");
		expect("$MyNic", Protocol::Unknown, "six bytes of a nick");
		CHECK(QuickDC::identify(0, 16) == Protocol::Unknown);
		printf("  ok too few bytes to tell is not a guess\n");
	}

	/* A TLS record needs all sixteen bytes before it is one. */
	{
		const std::string hello = clientHello(3, 3, 3);
		for (size_t n = 0; n < 16; n++)
			expect(hello.substr(0, n), Protocol::Unknown, "a truncated TLS record");

		expect(hello, Protocol::TLS, "the whole record");
		printf("  ok a partial TLS record is not yet a TLS record\n");
	}

	/* Anything else at all. */
	{
		expect("hello?\r\n", Protocol::Unknown, "chatter");
		expect("SSH-2.0-OpenSSH_9.0\r\n", Protocol::Unknown, "an SSH banner");
		expect("\x16\x03\x01", Protocol::Unknown, "three bytes of a TLS record");
		expect(std::string(64, '\0'), Protocol::Unknown, "a run of NULs");
		printf("  ok anything else is unknown\n");
	}

	/* The greeting has to be at the start, not merely somewhere in the data. */
	{
		expect(" CSUP ADBASE", Protocol::Unknown, "a leading space");
		expect("xxCSUP ADBASE", Protocol::Unknown, "a greeting further in");
		expect("\r\nGET / HTTP/1.1\r\n", Protocol::Unknown, "a leading blank line");
		printf("  ok a greeting only counts at the start\n");
	}

	/* Datagrams are told apart by their four character code alone. */
	{
		CHECK(QuickDC::identifyDatagram("URES ...", 8) == Protocol::ADC);
		CHECK(QuickDC::identifyDatagram("USCH ...", 8) == Protocol::ADC);
		CHECK(QuickDC::identifyDatagram("USTA ...", 8) == Protocol::ADC);
		CHECK(QuickDC::identifyDatagram("UPSR ...", 8) == Protocol::ADC);
		printf("  ok the ADC datagrams meant for us are recognised\n");
	}

	/* NMDC's two. */
	{
		CHECK(QuickDC::identifyDatagram("$SR bob file", 12) == Protocol::DC);
		CHECK(QuickDC::identifyDatagram("$Ping", 5) == Protocol::DC);
		printf("  ok the NMDC datagrams are recognised\n");
	}

	/* And nothing else. */
	{
		CHECK(QuickDC::identifyDatagram("BINF AAAA", 9) == Protocol::Unknown);
		CHECK(QuickDC::identifyDatagram("CSUP", 4) == Protocol::Unknown);
		CHECK(QuickDC::identifyDatagram("junk", 4) == Protocol::Unknown);
		CHECK(QuickDC::identifyDatagram("URE", 3) == Protocol::Unknown);
		CHECK(QuickDC::identifyDatagram("", 0) == Protocol::Unknown);
		CHECK(QuickDC::identifyDatagram(0, 8) == Protocol::Unknown);

		const char high[] = { (char) 0xff, (char) 0xff, (char) 0xff, (char) 0xff };
		CHECK(QuickDC::identifyDatagram(high, sizeof(high)) == Protocol::Unknown);
		printf("  ok a datagram we have no use for is unknown\n");
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
