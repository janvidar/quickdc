#include <string.h>
#include <string>

#include "network/user.h"
#include "network/usermanager.h"

/* A CID is the same shape as a TTH root: 39 base32 characters. */
#define QUEUE_TEST_CID_A "ACSTOWQXZASAQ7EYRJ4KLMNPQ2345XYZABCDEFG"
#define QUEUE_TEST_CID_B "Q2AH7321WAS1PQRSTUVWXYZ234567ABCDEFGHIJ"

EXO_TEST(user_1, {
	QuickDC::User u("id", "nick", "description", "email", 42ULL);
	
	return
			(
				strcmp(u.getID(), "id") == 0 &&
				strcmp(u.getNick(), "nick") == 0 &&
				strcmp(u.getDescription(), "description") == 0 &&
				strcmp(u.getEmail(), "email") == 0 &&
				u.getSharedBytes() == 42ULL
			);
});

EXO_TEST(user_2, {
	QuickDC::User u("id");
	
	return
			(
				strcmp(u.getID(), "id") == 0
			);
});


EXO_TEST(user_3, {
	QuickDC::User u("id");
	QuickDC::User u2(u);
	
	return
			(
				strcmp(u2.getID(), "id") == 0
			);
});

EXO_TEST(user_4, {
	QuickDC::User u("id");
	u.setNick("my nick");
	return (strcmp(u.getNick(), "my nick") == 0);
});

EXO_TEST(user_5, {
	QuickDC::User u("id");
	u.setDescription("my desc");
	return (strcmp(u.getDescription(), "my desc") == 0);
});

EXO_TEST(user_6, {
	QuickDC::User u("id");
	u.setEmail("my email");
	return (strcmp(u.getEmail(), "my email") == 0);
});

EXO_TEST(user_7, {
	QuickDC::User u("id");
	u.setSharedBytes(1233215484ULL);
	return (u.getSharedBytes() == 1233215484ULL);
});

EXO_TEST(user_8, {
	QuickDC::User u("id");
	u.setSpeed("my speed");
	return (strcmp(u.getSpeed(), "my speed") == 0);
});

EXO_TEST(user_9, {
	QuickDC::User u("id");
	u.setUserAgent("my user agent");
	return (strcmp(u.getUserAgent(), "my user agent") == 0);
});

EXO_TEST(user_10, {
	QuickDC::User u("id");
	u.setClientID((uint8_t*) "my ClientID");
	return (strcmp((const char*) u.getClientID(), "my ClientID") == 0);
});

EXO_TEST(user_11, {
	QuickDC::User u("id");
	u.setPrivateID((uint8_t*) "my PrivateID");
	return (strcmp((const char*) u.getPrivateID(), "my PrivateID") == 0);
});

EXO_TEST(user_12, {
	QuickDC::User u("id");
	u.setSessionID(884641);
	return u.getSessionID() == 884641;
});

EXO_TEST(user_13, {
	QuickDC::User u("id");
	u.setNumSlots(561);
	return u.getNumSlots() == 561;
});

EXO_TEST(user_14, {
	QuickDC::User u("id");
	u.setSharedFiles(87523);
	return u.getSharedFiles() == 87523;
});

EXO_TEST(user_15, {
	QuickDC::User u("id");
	u.setHubsOperator(123);
	return u.getHubsOperator() == 123;
});

EXO_TEST(user_16, {
	QuickDC::User u("id");
	u.setHubsRegular(586);
	return u.getHubsRegular() == 586;
});

EXO_TEST(user_17, {
	QuickDC::User u("id");
	u.setHubsRegistered(229);
	return u.getHubsRegistered() == 229;
});

EXO_TEST(user_18, {
	QuickDC::User u("id");
	u.setMaxSpeedUpload(7298456);
	return u.getMaxSpeedUpload() == 7298456;
});

EXO_TEST(user_19, {
	QuickDC::User u("id");
	u.setMaxSpeedDownload(5648412);
	return u.getMaxSpeedDownload() == 5648412;
});

EXO_TEST(user_20, {
	QuickDC::User u("id");
	u.setAutoSlotSpeed(6546);
	return u.getAutoSlotSpeed() == 6546;
});

EXO_TEST(user_21, {
	QuickDC::User u("id");
	u.setAutoSlotLimit(29921);
	return u.getAutoSlotLimit() == 29921;
});

EXO_TEST(user_22, {
	QuickDC::User u("id");
	u.setPortIPv4(1411);
	return u.getPortIPv4() == 1411;
});

EXO_TEST(user_23, {
	QuickDC::User u("id");
	u.setPortIPv6(1611);
	return u.getPortIPv6() == 1611;
});

EXO_TEST(user_24, {
	QuickDC::User u("id");
	u.setAddressIPv4("192.168.135.237");
	return strcmp(u.getAddressIPv4(), "192.168.135.237") == 0;
});

EXO_TEST(user_25, {
	QuickDC::User u("id");
	u.setAddressIPv6("fe80::201:2ff:fefa:f34e");
	return strcmp(u.getAddressIPv6(), "fe80::201:2ff:fefa:f34e") == 0;
});

EXO_TEST(usermanager_lookup_by_client_id, {
	/*
	 * The queue stores a client ID, because that is what identifies an ADC user
	 * across sessions. getUser() indexes by session ID instead, so looking a
	 * queued source up needs this - without it the scheduler could never
	 * resolve anyone and no download ever started.
	 */
	QuickDC::UserManager users(0);

	users.join("AAAA");
	users.join("BBBB");

	QuickDC::User* first = (QuickDC::User*) users.getUser("AAAA");
	QuickDC::User* second = (QuickDC::User*) users.getUser("BBBB");
	if (!first || !second) return false;

	/* setClientID() copies a fixed 40 bytes, so these have to be real length
	   CIDs - 39 base32 characters and a terminator. */
	first->setClientID((uint8_t*) QUEUE_TEST_CID_A);
	second->setClientID((uint8_t*) QUEUE_TEST_CID_B);
	first->setNick("alice");

	/* The session ID and the client ID are different keys. */
	if (users.getUserByClientID(QUEUE_TEST_CID_A) != first) return false;
	if (users.getUserByClientID(QUEUE_TEST_CID_B) != second) return false;
	if (users.getUser(QUEUE_TEST_CID_A) != 0) return false;

	/* And a client ID nobody has must not match anyone. */
	if (users.getUserByClientID("NOSUCHCID") != 0) return false;
	if (users.getUserByClientID("") != 0) return false;
	if (users.getUserByClientID(0) != 0) return false;

	/* The nick lookup still works, which is the NMDC path. */
	return users.getUserByNick("alice") == first;
});

/* ------------------------------------------------------------------------- *
 * Remote input handling
 *
 * Every string on a User arrives from a hub or a peer, so none of the lengths
 * are ours to assume.
 * ------------------------------------------------------------------------- */

EXO_TEST(user_long_strings_stay_terminated, {
	QuickDC::User u("AAAA");

	/* 'nick' is 64 bytes; anything at least that long used to be stored with
	   no terminator at all. */
	std::string huge(200, 'n');
	u.setNick(huge.c_str());

	const char* nick = u.getNick();
	if (!nick) return false;
	if (strlen(nick) != 63) return false;
	for (size_t n = 0; n < 63; n++)
		if (nick[n] != 'n') return false;

	/* 'speed' is only 16 bytes, so it is the easiest one to overrun. */
	u.setSpeed(std::string(64, 's').c_str());
	if (strlen(u.getSpeed()) != 15) return false;

	u.setDescription(std::string(1000, 'd').c_str());
	if (strlen(u.getDescription()) != 255) return false;

	u.setEmail(std::string(1000, 'e').c_str());
	return strlen(u.getEmail()) == 127;
});

EXO_TEST(user_long_useragent_does_not_reach_clientid, {
	/*
	 * userAgent is declared immediately before clientID. While a full length
	 * userAgent went in unterminated, reading the client ID ran on into it,
	 * which is what the download queue resolves its sources by.
	 */
	QuickDC::User u("AAAA");
	u.setClientID((uint8_t*) QUEUE_TEST_CID_A);

	u.setUserAgent(std::string(64, 'V').c_str());
	if (strlen(u.getUserAgent()) != 63) return false;

	const char* cid = (const char*) u.getClientID();
	if (!cid) return false;

	/* The client ID has to be exactly what was set, and nothing more. */
	return strcmp(cid, QUEUE_TEST_CID_A) == 0;
});

EXO_TEST(user_short_client_id_is_not_overread, {
	/*
	 * The copy used to take a fixed 40 bytes whatever the argument's length
	 * was, reading past the end of anything shorter. The stored value must be
	 * the string that was passed, terminated.
	 */
	QuickDC::User u("AAAA");

	u.setClientID((uint8_t*) "SHORT");
	if (strcmp((const char*) u.getClientID(), "SHORT")) return false;

	/* A full length CID still round trips. */
	u.setClientID((uint8_t*) QUEUE_TEST_CID_B);
	if (strcmp((const char*) u.getClientID(), QUEUE_TEST_CID_B)) return false;

	/* And a null clears it rather than crashing. */
	u.setClientID(0);
	if (*u.getClientID() != 0) return false;

	u.setPrivateID((uint8_t*) "ALSOSHORT");
	return strcmp((const char*) u.getPrivateID(), "ALSOSHORT") == 0;
});




