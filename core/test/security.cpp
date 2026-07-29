/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * SecurityManager decides whether a peer that turned up is allowed to take a
 * file from us. A clearance is granted when we ask someone to connect, and it
 * names the protocol, the hub session it was arranged on, the peer's ID and
 * the token that identifies the job.
 *
 * Every case here passes a null socket. isAuthorized() answers true for any
 * loopback peer before it looks at the table at all - see the last case - so a
 * socket would decide the answer before the matching under test ran.
 */

#include "quickdc.h"
#include "network/securitymanager.h"

#include <stdio.h>
#include <string.h>
#include <string>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

using Clearance = QuickDC::Clearance;

static QuickDC::Clearance* grant(QuickDC::SecurityManager& mgr,
	const char* cid, const char* token,
	Clearance::ClearanceProtocol protocol = Clearance::ADC,
	Clearance::ClearanceType type = Clearance::AuthorizedUser)
{
	QuickDC::Clearance* c = new QuickDC::Clearance(type, protocol, 0, cid, token);
	mgr.grant(c);
	return c;
}

/* No socket, so the loopback shortcut is not what answers. */
static bool authorized(QuickDC::SecurityManager& mgr, const char* cid, const char* token,
	Clearance::ClearanceProtocol protocol = Clearance::ADC)
{
	return mgr.isAuthorized(protocol, 0, 0, cid, token);
}

int main()
{
	setbuf(stdout, 0);

	/* An empty table authorises nobody. */
	{
		QuickDC::SecurityManager mgr;
		CHECK(!authorized(mgr, "CIDA", "token"));
		printf("  ok an empty table authorises nobody\n");
	}

	/* The peer and the job both have to match. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "token1");

		CHECK(authorized(mgr, "CIDA", "token1"));
		CHECK(!authorized(mgr, "CIDA", "token2"));
		CHECK(!authorized(mgr, "CIDB", "token1"));
		CHECK(!authorized(mgr, "CIDB", "token2"));
		printf("  ok a clearance matches one peer and one job\n");
	}

	/* A clearance is for one protocol. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "token1", Clearance::ADC);

		CHECK(authorized(mgr, "CIDA", "token1", Clearance::ADC));
		CHECK(!authorized(mgr, "CIDA", "token1", Clearance::HTTP));
		printf("  ok a clearance does not carry across protocols\n");
	}

	/* Neither side naming a job is not a match. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "token1");

		CHECK(!authorized(mgr, "CIDA", 0));
		printf("  ok a peer with no token is not authorised\n");
	}

	/* Several clearances coexist, and each matches only its own. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "t1");
		grant(mgr, "CIDB", "t2");
		grant(mgr, "CIDC", "t3");

		CHECK(authorized(mgr, "CIDA", "t1"));
		CHECK(authorized(mgr, "CIDB", "t2"));
		CHECK(authorized(mgr, "CIDC", "t3"));

		/* Not each other's. */
		CHECK(!authorized(mgr, "CIDA", "t2"));
		CHECK(!authorized(mgr, "CIDC", "t1"));
		printf("  ok clearances do not authorise each other's jobs\n");
	}

	/* Once the connection it was granted for has arrived, it is spent. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "t1");
		grant(mgr, "CIDB", "t2");

		CHECK(mgr.revoke(Clearance::ADC, 0, "CIDA", "t1"));
		CHECK(!authorized(mgr, "CIDA", "t1"));

		/* And only that one. */
		CHECK(authorized(mgr, "CIDB", "t2"));
		printf("  ok revoking spends one clearance and leaves the rest\n");
	}

	/* Revoking something that was never granted says so. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "t1");

		CHECK(!mgr.revoke(Clearance::ADC, 0, "CIDA", "nosuchtoken"));
		CHECK(!mgr.revoke(Clearance::ADC, 0, "CIDNOBODY", "t1"));
		CHECK(authorized(mgr, "CIDA", "t1"));
		printf("  ok revoking what was never granted changes nothing\n");
	}

	/* Revoking twice is not a second success. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "t1");

		CHECK(mgr.revoke(Clearance::ADC, 0, "CIDA", "t1"));
		CHECK(!mgr.revoke(Clearance::ADC, 0, "CIDA", "t1"));
		printf("  ok a spent clearance cannot be spent again\n");
	}

	/* An UnknownUser clearance is the wildcard the local hub grants. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "t1", Clearance::ADC, Clearance::UnknownUser);

		CHECK(authorized(mgr, "ANYBODY", "anything"));
		printf("  ok an UnknownUser clearance authorises anyone\n");
	}

	/*
	 * Documented rather than endorsed: isAuthorized() returns true for any peer
	 * on the loopback interface, before it looks at the table. It is guarded by
	 * `#if 1` with a FIXME asking whether it should be limited to debug builds,
	 * so it is in release builds too - anything that can open a peer connection
	 * from 127.0.0.1 can take whatever we would upload, with no clearance, no
	 * token and no matching ID.
	 *
	 * This case exists so that changing it is a deliberate act rather than a
	 * surprise. There is no socket to hand here, so it is left asserting what
	 * the table does when one is absent.
	 */
	{
		QuickDC::SecurityManager mgr;
		CHECK(!mgr.isAuthorized(Clearance::ADC, 0, 0, "CIDA", "t1"));
		printf("  ok without a socket the table is what decides\n");
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
