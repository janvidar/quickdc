/*
 * QuickDC::FileSource: the source URL notation documented in filesource.h, and
 * the retry and age counters that decide when a source has stopped being worth
 * asking.
 */

#include <samurai/samurai.h>
#include <string.h>
#include <stdlib.h>

#include "network/filesource.h"

#define QUEUE_TEST_TTH_A "MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"

/*
 * These tables live out here on purpose: EXO_TEST() takes the test body as a
 * single macro argument, so a brace-enclosed initializer list inside one would
 * be split on its commas and read as extra arguments.
 */

/** Every source URL form documented in filesource.h. */
static const char* queue_test_urls[] = {
	"adc://adchub:7817/ACSTOWQXZASA/TTH:" QUEUE_TEST_TTH_A,
	"dchub://myhub:411/NBSWY3DP/file.bin",
	"adc://host:7817/CID/dir/",   /* a directory */
	"adc://host:7817/CID/",       /* the root directory */
	"adc://host:7817/CID",        /* the file list */
	"adcs://host:7817/CID/deep/path/f.iso",
	0
};

/** Source URLs that carry no usable hub and client ID. */
static const char* queue_test_bad_urls[] = {
	"",
	"notaurl",
	"adc://host:7817",     /* no client ID */
	"adc://host:7817/",    /* empty client ID */
	"://host/CID",         /* no protocol */
	"adc:/host/CID",       /* not a URL */
	0
};

EXO_TEST(filesource_url_roundtrip, {
	/* Every form documented in filesource.h has to survive decode/encode. */
	for (size_t n = 0; queue_test_urls[n]; n++)
	{
		QuickDC::FileSource src(queue_test_urls[n]);
		if (!src.isValid()) return false;

		char* out = src.toURL();
		if (!out) return false;

		bool ok = (strcmp(out, queue_test_urls[n]) == 0);
		free(out);
		if (!ok) return false;
	}
	return true;
});

EXO_TEST(filesource_splits_parts, {
	QuickDC::FileSource src("adc://adchub:7817/ACSTOWQXZASA/TTH:ABC");
	return src.isValid()
		&& !strcmp(src.getHubUrl(), "adc://adchub:7817")
		&& !strcmp(src.getClientID(), "ACSTOWQXZASA")
		&& !strcmp(src.getName(), "TTH:ABC")
		&& !src.isFilelist();
});

EXO_TEST(filesource_filelist_vs_root_directory, {
	/* No trailing slash means the file list; a trailing slash means the
	   root directory. The difference is a null versus empty filename. */
	QuickDC::FileSource list("adc://host:7817/CID");
	QuickDC::FileSource root("adc://host:7817/CID/");

	return list.isFilelist() && list.getName() == 0
		&& !root.isFilelist() && root.getName() && !strcmp(root.getName(), "");
});

EXO_TEST(filesource_rejects_malformed, {
	for (size_t n = 0; queue_test_bad_urls[n]; n++)
	{
		QuickDC::FileSource src(queue_test_bad_urls[n]);
		if (src.isValid()) return false;
		if (src.toURL() != 0) return false;
	}

	QuickDC::FileSource nil((const char*) 0);
	return !nil.isValid();
});

EXO_TEST(filesource_ctor_agrees_with_url, {
	QuickDC::FileSource src("adc://host:7817", "CID", "file.bin");
	char* out = src.toURL();
	bool ok = (out && !strcmp(out, "adc://host:7817/CID/file.bin"));
	free(out);
	return ok;
});

/* A CID is the peer on every ADC hub; an NMDC nickname only on its own hub, however that is spelled. */
EXO_TEST(filesource_matches, {
	QuickDC::FileSource src("adc://host:7817/CID/f");
	QuickDC::FileSource nick("dchub://host:411/bob/f");
	return src.matches("adc://host:7817", "CID")
		&& src.matches("adcs://other:7817", "CID")
		&& !src.matches("adc://host:7817", "OTHER")
		&& !src.matches("dchub://host:7817", "CID")
		&& nick.matches("DCHUB://Host:411", "bob")
		&& nick.matches(" dchub://host ", "bob")
		&& !nick.matches("dchub://other:411", "bob")
		&& !nick.matches("dchub://host:411", "Bob")
		&& !src.matches(0, 0);
});

EXO_TEST(filesource_expires_after_retries, {
	QuickDC::FileSource src("adc://host:7817/CID/f");
	src.touch();
	if (src.isExpired()) return false;

	for (int n = 0; n <= QUICKDC_SOURCE_MAX_RETRIES; n++)
		src.onFailure();

	if (!src.isExpired()) return false;

	/* Being seen again clears the slate. */
	src.touch();
	return !src.isExpired() && src.getRetryCount() == 0;
});

EXO_TEST(filesource_expires_when_stale, {
	QuickDC::FileSource src("adc://host:7817/CID/f");
	src.touch();
	if (src.isExpired()) return false;

	time_t old = Samurai::TimeStamp().getInternalData() - (2 * QUICKDC_SOURCE_MAX_AGE);
	src.setLastSeen(Samurai::TimeStamp(old));
	return src.isExpired();
});
