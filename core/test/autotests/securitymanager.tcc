/*
 * QuickDC::SecurityManager: the table of clearances that says which incoming
 * peer connection was asked for, and what granting, revoking and consuming one
 * does to the rest of the table.
 */

#include "network/securitymanager.h"

/* A CID is the same shape as a TTH root: 39 base32 characters. */
#define QUEUE_TEST_CID_A "ACSTOWQXZASAQ7EYRJ4KLMNPQ2345XYZABCDEFG"
#define QUEUE_TEST_CID_B "Q2AH7321WAS1PQRSTUVWXYZ234567ABCDEFGHIJ"

EXO_TEST(securitymanager_revoke_by_session, {
	/*
	 * revoke(HubSession*) erased while iterating and then incremented the dead
	 * iterator, so it ran off the vector as soon as it matched. The sessions
	 * are only ever compared, never dereferenced, so opaque tags will do.
	 */
	QuickDC::SecurityManager security;

	QuickDC::HubSession* doomed = (QuickDC::HubSession*) 0x1;
	QuickDC::HubSession* keep = (QuickDC::HubSession*) 0x2;

	/* Several consecutive entries for the same session is the case that broke. */
	for (int n = 0; n < 4; n++)
	{
		security.grant(new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
			QuickDC::Clearance::ADC, doomed, QUEUE_TEST_CID_A, "doomedtoken"));
	}
	security.grant(new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
		QuickDC::Clearance::ADC, keep, QUEUE_TEST_CID_B, "keeptoken"));

	/* Passing no socket keeps the loopback shortcut out of the way. */
	if (!security.isAuthorized(QuickDC::Clearance::ADC, doomed, 0, QUEUE_TEST_CID_A, "doomedtoken"))
		return false;

	security.revoke(doomed);

	/* Every entry for that session is gone, and the other one survived. */
	if (security.isAuthorized(QuickDC::Clearance::ADC, doomed, 0, QUEUE_TEST_CID_A, "doomedtoken"))
		return false;

	return security.isAuthorized(QuickDC::Clearance::ADC, keep, 0, QUEUE_TEST_CID_B, "keeptoken");
});

EXO_TEST(securitymanager_revoke_one_connection, {
	/*
	 * A clearance covers a single connection request. Once that connection has
	 * turned up it must be removed, or the table grows by one entry for every
	 * request the process ever makes.
	 */
	QuickDC::SecurityManager security;

	QuickDC::HubSession* session = (QuickDC::HubSession*) 0x1;

	security.grant(new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
		QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_A, "tokenA"));
	security.grant(new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
		QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_B, "tokenB"));

	if (!security.isAuthorized(QuickDC::Clearance::ADC, session, 0, QUEUE_TEST_CID_A, "tokenA"))
		return false;

	/* Consuming one leaves the other alone. */
	if (!security.revoke(QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_A, "tokenA"))
		return false;
	if (security.isAuthorized(QuickDC::Clearance::ADC, session, 0, QUEUE_TEST_CID_A, "tokenA"))
		return false;
	if (!security.isAuthorized(QuickDC::Clearance::ADC, session, 0, QUEUE_TEST_CID_B, "tokenB"))
		return false;

	/* Revoking the same thing twice finds nothing the second time. */
	if (security.revoke(QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_A, "tokenA"))
		return false;

	/* A wrong token or user must not revoke anything. */
	if (security.revoke(QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_B, "wrongtoken"))
		return false;
	if (security.revoke(QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_A, "tokenB"))
		return false;
	if (security.revoke(QuickDC::Clearance::ADC, session, 0, 0))
		return false;

	/* A null session matches any, the way isAuthorized() treats it. */
	if (!security.revoke(QuickDC::Clearance::ADC, 0, QUEUE_TEST_CID_B, "tokenB"))
		return false;

	return !security.isAuthorized(QuickDC::Clearance::ADC, session, 0, QUEUE_TEST_CID_B, "tokenB");
});

EXO_TEST(securitymanager_keeps_blanket_clearance, {
	/*
	 * An UnknownUser clearance is not tied to one connection, so consuming a
	 * connection must not take it away.
	 */
	QuickDC::SecurityManager security;
	QuickDC::HubSession* session = (QuickDC::HubSession*) 0x1;

	security.grant(new QuickDC::Clearance(QuickDC::Clearance::UnknownUser,
		QuickDC::Clearance::ADC, session, 0, 0));

	if (security.revoke(QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_A, "anytoken"))
		return false;

	/* Still open to everyone, as granted. */
	return security.isAuthorized(QuickDC::Clearance::ADC, session, 0, QUEUE_TEST_CID_A, "anytoken");
});
