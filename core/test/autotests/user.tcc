#include "network/user.h"

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




