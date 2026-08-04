/*
 * QuickDC::FilelistRequest: what may name a downloaded file list, and the names
 * the request derives from it.
 *
 * The identifier is a CID only when the hub published one and it verified.
 * Otherwise it is the nickname, which is 63 arbitrary bytes off the wire, and
 * it is what the list is named after - so it decides a path.
 */

#include <string.h>

#include "network/downloadqueue.h"

/* A CID is the same shape as a TTH root: 39 base32 characters. */
#define QUEUE_TEST_CID_A "ACSTOWQXZASAQ7EYRJ4KLMNPQ2345XYZABCDEFG"
#define QUEUE_TEST_CID_B "Q2AH7321WAS1PQRSTUVWXYZ234567ABCDEFGHIJ"

/*
 * These tables live out here on purpose: EXO_TEST() takes the test body as a
 * single macro argument, so a brace-enclosed initializer list inside one would
 * be split on its commas and read as extra arguments.
 */

/**
 * Identifiers that must never be allowed to name a downloaded file list. Every
 * one of them is a nickname a hub is free to advertise.
 */
static const char* queue_test_bad_identifiers[] = {
	"",
	"..",
	".",
	"../../../../home/user/.config/autostart/x",
	"a/b",
	"a\\b",
	"/etc/passwd",
	"x..y",        /* two dots anywhere at all */
	"a\nb",        /* control bytes */
	"a\x7f",
	"C:name",
	/* Longer than any CID or nickname can be: 80 characters. */
	QUEUE_TEST_CID_A QUEUE_TEST_CID_B "AA",
	0
};

EXO_TEST(filelist_identifier_accepts_cid_and_nick, {
	if (!QuickDC::FilelistRequest::isValidIdentifier(QUEUE_TEST_CID_A)) return false;
	if (!QuickDC::FilelistRequest::isValidIdentifier("alice")) return false;
	if (!QuickDC::FilelistRequest::isValidIdentifier("[NL]Bj\xc3\xb8rn_1")) return false;

	/* A dot is ordinary in a nickname; only "." alone and ".." are not. */
	return QuickDC::FilelistRequest::isValidIdentifier("some.one");
});

EXO_TEST(filelist_identifier_rejects_paths, {
	for (size_t n = 0; queue_test_bad_identifiers[n]; n++)
		if (QuickDC::FilelistRequest::isValidIdentifier(queue_test_bad_identifiers[n]))
			return false;

	return !QuickDC::FilelistRequest::isValidIdentifier(0);
});

EXO_TEST(filelist_request_refuses_to_name_a_path, {
	/*
	 * The request is what turns the identifier into a file name, so it has to
	 * refuse one by itself - the callers are protocol code and this program.
	 */
	QuickDC::FilelistRequest bad("adc://h:1", "../../../../tmp/quickdc_traversal");
	if (bad.isValid()) return false;
	if (bad.getLocalFilename() != 0 || bad.getTempFilename() != 0) return false;

	QuickDC::FilelistRequest good("adc://h:1", QUEUE_TEST_CID_A, "/tmp");
	return good.isValid()
		&& !strcmp(good.getLocalFilename(), "/tmp/" QUEUE_TEST_CID_A ".xml")
		&& !strcmp(good.getTempFilename(), "/tmp/" QUEUE_TEST_CID_A ".xml" QUICKDC_PART_SUFFIX);
});
