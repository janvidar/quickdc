#include "network/keyprint.h"
#include <samurai/io/net/url.h>
#include <string.h>

/* ------------------------------------------------------------------------- *
 * QuickDC::Keyprint
 *
 * A keyprint arrives either in a hub address the user typed or in an INF field
 * a hub relayed, so both malformed and hostile input is to be expected. What
 * matters most is that nothing malformed ever compares equal to anything.
 * ------------------------------------------------------------------------- */

#define KEYPRINT_A "SHA256/34PTPCDDEKHBRR2FLP45VXGUIZREQG6DQOMAUR65LBIZHTXNMV5Q"
#define KEYPRINT_B "SHA256/BT37EIR3TJJQJPGXFKVJXLUAMO24SP5OMLIOWUOC5KRZ7C7KTSBA"

EXO_TEST(keyprint_valid, {
	return QuickDC::Keyprint::isValid(KEYPRINT_A);
});

EXO_TEST(keyprint_valid_lowercase, {
	return QuickDC::Keyprint::isValid("sha256/34ptpcddekhbrr2flp45vxguizreqg6dqomaur65lbizhtxnmv5q");
});

EXO_TEST(keyprint_invalid_null, {
	return !QuickDC::Keyprint::isValid(0);
});

EXO_TEST(keyprint_invalid_empty, {
	return !QuickDC::Keyprint::isValid("");
});

EXO_TEST(keyprint_invalid_hash, {
	return !QuickDC::Keyprint::isValid("SHA1/34PTPCDDEKHBRR2FLP45VXGUIZREQG6DQOMAUR65LBIZHTXNMV5Q");
});

EXO_TEST(keyprint_invalid_no_hash_name, {
	return !QuickDC::Keyprint::isValid("34PTPCDDEKHBRR2FLP45VXGUIZREQG6DQOMAUR65LBIZHTXNMV5Q");
});

EXO_TEST(keyprint_invalid_too_short, {
	return !QuickDC::Keyprint::isValid("SHA256/34PTPCDDEKHBRR2FLP45VXGUIZREQG6DQOMAUR65LBIZHTXNMV5");
});

EXO_TEST(keyprint_invalid_too_long, {
	return !QuickDC::Keyprint::isValid("SHA256/34PTPCDDEKHBRR2FLP45VXGUIZREQG6DQOMAUR65LBIZHTXNMV5QQ");
});

/* '0', '1', '8' and '9' are not in the base32 alphabet. */
EXO_TEST(keyprint_invalid_alphabet, {
	return !QuickDC::Keyprint::isValid("SHA256/01PTPCDDEKHBRR2FLP45VXGUIZREQG6DQOMAUR65LBIZHTXNMV5Q");
});

EXO_TEST(keyprint_invalid_padding, {
	return !QuickDC::Keyprint::isValid("SHA256/34PTPCDDEKHBRR2FLP45VXGUIZREQG6DQOMAUR65LBIZHTXNMV5=");
});

EXO_TEST(keyprint_equals_same, {
	return QuickDC::Keyprint::equals(KEYPRINT_A, KEYPRINT_A);
});

EXO_TEST(keyprint_equals_case, {
	return QuickDC::Keyprint::equals(KEYPRINT_A,
		"sha256/34ptpcddekhbrr2flp45vxguizreqg6dqomaur65lbizhtxnmv5q");
});

EXO_TEST(keyprint_equals_different, {
	return !QuickDC::Keyprint::equals(KEYPRINT_A, KEYPRINT_B);
});

/* One character apart is still a different certificate. */
EXO_TEST(keyprint_equals_off_by_one, {
	return !QuickDC::Keyprint::equals(KEYPRINT_A,
		"SHA256/34PTPCDDEKHBRR2FLP45VXGUIZREQG6DQOMAUR65LBIZHTXNMV5A");
});

EXO_TEST(keyprint_equals_malformed, {
	return !QuickDC::Keyprint::equals(KEYPRINT_A, "SHA256/nonsense");
});

EXO_TEST(keyprint_equals_null, {
	return !QuickDC::Keyprint::equals(KEYPRINT_A, 0) &&
	       !QuickDC::Keyprint::equals(0, KEYPRINT_A) &&
	       !QuickDC::Keyprint::equals(0, 0);
});

EXO_TEST(keyprint_query_only, {
	std::string kp;
	return QuickDC::Keyprint::fromQuery("kp=" KEYPRINT_A, kp) && kp == KEYPRINT_A;
});

EXO_TEST(keyprint_query_first, {
	std::string kp;
	return QuickDC::Keyprint::fromQuery("kp=" KEYPRINT_A "&foo=bar", kp) && kp == KEYPRINT_A;
});

EXO_TEST(keyprint_query_last, {
	std::string kp;
	return QuickDC::Keyprint::fromQuery("foo=bar&kp=" KEYPRINT_A, kp) && kp == KEYPRINT_A;
});

EXO_TEST(keyprint_query_none, {
	std::string kp;
	return !QuickDC::Keyprint::fromQuery("", kp);
});

EXO_TEST(keyprint_query_other_key, {
	std::string kp;
	return !QuickDC::Keyprint::fromQuery("kpx=" KEYPRINT_A, kp) &&
	       !QuickDC::Keyprint::fromQuery("k=" KEYPRINT_A, kp) &&
	       !QuickDC::Keyprint::fromQuery("akp=" KEYPRINT_A, kp);
});

/* Unusable, but still present: the two mean different things to the caller. */
EXO_TEST(keyprint_query_malformed_value, {
	std::string kp;
	return QuickDC::Keyprint::fromQuery("kp=SHA256/nonsense", kp) &&
	       !QuickDC::Keyprint::isValid(kp.c_str());
});

/* ------------------------------------------------------------------------- *
 * Samurai::IO::Net::URL
 *
 * A keyprint sits in the query, and its base32 contains a '/', which used to
 * be mistaken for the start of the path and taken into the port.
 * ------------------------------------------------------------------------- */

EXO_TEST(keyprint_url_with_path, {
	Samurai::IO::Net::URL url("adcs://adcs.uhub.org:1511/?kp=" KEYPRINT_A);
	return url.isValid() &&
	       url.getScheme() == "adcs" &&
	       url.getHost().toString() == "adcs.uhub.org" &&
	       url.getPort() == 1511 &&
	       url.getQuery() == "kp=" KEYPRINT_A;
});

EXO_TEST(keyprint_url_without_path, {
	Samurai::IO::Net::URL url("adcs://adcs.uhub.org:1511?kp=" KEYPRINT_A);
	return url.isValid() &&
	       url.getPort() == 1511 &&
	       url.getQuery() == "kp=" KEYPRINT_A;
});

EXO_TEST(keyprint_url_no_query, {
	Samurai::IO::Net::URL url("adcs://adcs.uhub.org:1511");
	return url.isValid() && url.getPort() == 1511 && url.getQuery().empty();
});

EXO_TEST(keyprint_url_from_query, {
	Samurai::IO::Net::URL url("adcs://adcs.uhub.org:1511/?kp=" KEYPRINT_A);
	std::string kp;
	return QuickDC::Keyprint::fromQuery(url.getQuery(), kp) &&
	       QuickDC::Keyprint::equals(kp.c_str(), KEYPRINT_A);
});
