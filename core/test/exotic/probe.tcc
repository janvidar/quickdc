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

/*
 * Returns whether it matched, and says what went wrong if it did not - the
 * expected and actual protocol, which an assertion on the call alone would not
 * show. Callers wrap it in EXO_ASSERT.
 */
static bool expect(const std::string& data, Protocol want, const char* what)
{
	const Protocol got = identify(data);
	if (got == want) return true;

	printf("    %s: expected %s, got %s\n", what, name(want), name(got));
	return false;
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

/* The two ADC greetings, which differ only in who they are addressed to. */
EXO_TEST(probe_the_two_adc_greetings_are_told_apart,
{
		EXO_ASSERT(expect("CSUP ADBASE ADTIGR\n", Protocol::ADC, "a client's SUP"));
		EXO_ASSERT(expect("HSUP ADBASE ADTIGR\n", Protocol::ADCHub, "a hub's SUP"));
		EXO_ASSERT(expect("CSUP", Protocol::ADC, "a bare CSUP"));
	
	return 1;
});

/* NMDC announces itself with a nick. */
EXO_TEST(probe_an_nmdc_greeting_is_recognised,
{
		EXO_ASSERT(expect("$MyNick bob|", Protocol::DC, "a nick"));
		EXO_ASSERT(expect("$MyNick ", Protocol::DC, "the shortest nick line"));
	
	return 1;
});

/* The methods the HTTP server answers. */
EXO_TEST(probe_an_http_request_is_recognised,
{
		EXO_ASSERT(expect("GET / HTTP/1.1\r\n", Protocol::HTTP, "GET"));
		EXO_ASSERT(expect("HEAD / HTTP/1.1\r\n", Protocol::HTTP, "HEAD"));
		EXO_ASSERT(expect("POST / HTTP/1.1\r\n", Protocol::HTTP, "POST"));
		EXO_ASSERT(expect("PUT / HTTP/1.1\r\n", Protocol::HTTP, "PUT"));
		EXO_ASSERT(expect("OPTIONS * HTTP/1.1\r\n", Protocol::HTTP, "OPTIONS"));
		EXO_ASSERT(expect("TRACE / HTTP/1.1\r\n", Protocol::HTTP, "TRACE"));
		EXO_ASSERT(expect("DELETE / HTTP/1.1\r\n", Protocol::HTTP, "DELETE"));
	
	return 1;
});

/* And a method it does not, which is not a reason to guess. */
EXO_TEST(probe_a_method_we_do_not_answer_is_not_taken_for_http,
{
		EXO_ASSERT(expect("PATCH / HTTP/1.1\r\n", Protocol::Unknown, "PATCH"));
		EXO_ASSERT(expect("BREW /coffee HTTP/1.1\r\n", Protocol::Unknown, "BREW"));
	
	return 1;
});

/* A TLS handshake, which has to be finished before anything can be read. */
EXO_TEST(probe_a_tls_handshake_is_recognised,
{
		EXO_ASSERT(expect(clientHello(3, 1, 3), Protocol::TLS, "a TLS 1.0 record"));
		EXO_ASSERT(expect(clientHello(3, 3, 3), Protocol::TLS, "a TLS 1.2 record"));

		/*
		 * A TLS 1.3 client sends a 1.0 record carrying a 1.2 client version for
		 * middlebox compatibility, so the two are not required to agree.
		 */
		EXO_ASSERT(expect(clientHello(3, 1, 3), Protocol::TLS, "a TLS 1.3 record"));
	
	return 1;
});

/* Something that is not one. */
EXO_TEST(probe_a_record_that_is_not_a_client_handshake_is_refused,
{
		/* A record that is not a handshake. */
		std::string appdata = clientHello(3, 1, 3);
		appdata[0] = (char) 23;
		EXO_ASSERT(expect(appdata, Protocol::Unknown, "an application data record"));

		/* A handshake that is not a ClientHello. */
		std::string server = clientHello(3, 1, 3);
		server[5] = (char) 2;
		EXO_ASSERT(expect(server, Protocol::Unknown, "a ServerHello"));

		/* Something older than SSL 3.0. */
		std::string ancient = clientHello(2, 0, 3);
		EXO_ASSERT(expect(ancient, Protocol::Unknown, "an SSL 2.0 record version"));
	
	return 1;
});

/*
	 * The high bit set in the first byte is the old SSLv2 handshake, and it is
	 * also where a signed char stops behaving: every fourcc here is built from
	 * one, so a byte over 127 used to spread through the whole word.
	 */
EXO_TEST(probe_an_sslv2_handshake_is_recognised,
{
		std::string v2;
		v2 += (char) 0x80;   /* length, high bit set */
		v2 += (char) 0x2e;
		v2 += (char) 1;      /* ClientHello */
		v2 += (char) 3;      /* version major */
		v2 += (char) 1;
		while (v2.size() < 16) v2 += (char) 0x5a;
		EXO_ASSERT(expect(v2, Protocol::TLS, "an SSLv2 hello"));
	
	return 1;
});

/* Bytes over 127 anywhere in the fourcc are not any of ours. */
EXO_TEST(probe_a_byte_over_127_does_not_spread_through_the_fourcc,
{
		const char high[] = { (char) 0xc3, 'S', 'U', 'P', ' ', 'A', 'D', 'B', 'A', 'S', 'E' };
		EXO_ASSERT(expect(std::string(high, sizeof(high)), Protocol::Unknown, "a high byte first"));

		const char high2[] = { 'C', (char) 0xc3, 'U', 'P', ' ', 'A', 'D', 'B', 'A', 'S', 'E' };
		EXO_ASSERT(expect(std::string(high2, sizeof(high2)), Protocol::Unknown, "a high byte inside"));

		const char allhigh[] = { (char) 0xff, (char) 0xff, (char) 0xff, (char) 0xff,
			(char) 0xff, (char) 0xff, (char) 0xff, (char) 0xff };
		EXO_ASSERT(expect(std::string(allhigh, sizeof(allhigh)), Protocol::Unknown, "all high bytes"));
	
	return 1;
});

/* Too little to tell yet is not a protocol. */
EXO_TEST(probe_too_few_bytes_to_tell_is_not_a_guess,
{
		EXO_ASSERT(expect("", Protocol::Unknown, "nothing"));
		EXO_ASSERT(expect("C", Protocol::Unknown, "one byte"));
		EXO_ASSERT(expect("CSU", Protocol::Unknown, "three bytes of CSUP"));
		EXO_ASSERT(expect("$MyNic", Protocol::Unknown, "six bytes of a nick"));
		EXO_ASSERT(QuickDC::identify(0, 16) == Protocol::Unknown);
	
	return 1;
});

/* A TLS record needs all sixteen bytes before it is one. */
EXO_TEST(probe_a_partial_tls_record_is_not_yet_a_tls_record,
{
		const std::string hello = clientHello(3, 3, 3);
		for (size_t n = 0; n < 16; n++)
			EXO_ASSERT(expect(hello.substr(0, n), Protocol::Unknown, "a truncated TLS record"));

		EXO_ASSERT(expect(hello, Protocol::TLS, "the whole record"));
	
	return 1;
});

/* Anything else at all. */
EXO_TEST(probe_anything_else_is_unknown,
{
		EXO_ASSERT(expect("hello?\r\n", Protocol::Unknown, "chatter"));
		EXO_ASSERT(expect("SSH-2.0-OpenSSH_9.0\r\n", Protocol::Unknown, "an SSH banner"));
		EXO_ASSERT(expect("\x16\x03\x01", Protocol::Unknown, "three bytes of a TLS record"));
		EXO_ASSERT(expect(std::string(64, '\0'), Protocol::Unknown, "a run of NULs"));
	
	return 1;
});

/* The greeting has to be at the start, not merely somewhere in the data. */
EXO_TEST(probe_a_greeting_only_counts_at_the_start,
{
		EXO_ASSERT(expect(" CSUP ADBASE", Protocol::Unknown, "a leading space"));
		EXO_ASSERT(expect("xxCSUP ADBASE", Protocol::Unknown, "a greeting further in"));
		EXO_ASSERT(expect("\r\nGET / HTTP/1.1\r\n", Protocol::Unknown, "a leading blank line"));
	
	return 1;
});

/* Datagrams are told apart by their four character code alone. */
EXO_TEST(probe_the_adc_datagrams_meant_for_us_are_recognised,
{
		EXO_ASSERT(QuickDC::identifyDatagram("URES ...", 8) == Protocol::ADC);
		EXO_ASSERT(QuickDC::identifyDatagram("USCH ...", 8) == Protocol::ADC);
		EXO_ASSERT(QuickDC::identifyDatagram("USTA ...", 8) == Protocol::ADC);
		EXO_ASSERT(QuickDC::identifyDatagram("UPSR ...", 8) == Protocol::ADC);
	
	return 1;
});

/* NMDC's two. */
EXO_TEST(probe_the_nmdc_datagrams_are_recognised,
{
		EXO_ASSERT(QuickDC::identifyDatagram("$SR bob file", 12) == Protocol::DC);
		EXO_ASSERT(QuickDC::identifyDatagram("$Ping", 5) == Protocol::DC);
	
	return 1;
});

/* And nothing else. */
EXO_TEST(probe_a_datagram_we_have_no_use_for_is_unknown,
{
		EXO_ASSERT(QuickDC::identifyDatagram("BINF AAAA", 9) == Protocol::Unknown);
		EXO_ASSERT(QuickDC::identifyDatagram("CSUP", 4) == Protocol::Unknown);
		EXO_ASSERT(QuickDC::identifyDatagram("junk", 4) == Protocol::Unknown);
		EXO_ASSERT(QuickDC::identifyDatagram("URE", 3) == Protocol::Unknown);
		EXO_ASSERT(QuickDC::identifyDatagram("", 0) == Protocol::Unknown);
		EXO_ASSERT(QuickDC::identifyDatagram(0, 8) == Protocol::Unknown);

		const char high[] = { (char) 0xff, (char) 0xff, (char) 0xff, (char) 0xff };
		EXO_ASSERT(QuickDC::identifyDatagram(high, sizeof(high)) == Protocol::Unknown);
	
	return 1;
});
