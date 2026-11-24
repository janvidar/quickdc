/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The client identity. The PID is this client's long term secret and the CID
 * everyone sees is its hash, so two things have to hold: the pair has to be
 * generated unguessably, and it has to come back unchanged on the next start -
 * a client whose CID moves loses every hub registration it had.
 *
 * ADC::CID::verify() and the SID conversions are already covered by
 * adc_parser.tcc; what is here is the relationship between a CID and its PID,
 * and the identity surviving a restart.
 *
 * QuickDC::Core reads its configuration from disk, so it is a per-file fixture. Each case establishes whatever identity it needs rather than
 * inheriting one from the case before it: the identity lives in a file, and a
 * test that only passes because an earlier one wrote something is a test that
 * cannot be run on its own.
 */

#include "quickdc.h"
#include "api/core.h"
#include "config/preferences.h"
#include "network/adc/cid.h"

#include <samurai/crypto/digest/tiger.h>
#include <samurai/io/file.h>
#include <samurai/util/base32.h>

#include <memory>
#include <span>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>

/* The fixture's core; built in EXO_SETUP below. */
QuickDC::Core* core = 0;

#define CID_LENGTH 39

namespace {

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_cid_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

std::string readFile(const std::string& path)
{
	FILE* fh = fopen(path.c_str(), "rb");
	if (!fh) return std::string();

	std::string out;
	char buf[65536];
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), fh)) > 0) out.append(buf, got);
	fclose(fh);
	return out;
}

bool writeFile(const std::string& path, const std::string& content)
{
	FILE* fh = fopen(path.c_str(), "wb");
	if (!fh) return false;
	const bool ok = fwrite(content.data(), 1, content.size(), fh) == content.size();
	fclose(fh);
	return ok;
}

std::unique_ptr<ADC::CID> held_identity;

/* An identity read from the fixture's configuration, as a core starting up would. */
ADC::CID* freshIdentity()
{
	held_identity.reset();
	held_identity = std::make_unique<ADC::CID>(*core->config);
	return held_identity.get();
}

/*
 * The CID a PID implies, worked out here rather than asked for: the Tiger hash
 * of the decoded private ID, in base32.
 */
std::string cidOf(const char* pid)
{
	uint8_t raw[Samurai::Crypto::Digest::TIGER_HASH_SIZE] = { 0, };
	Samurai::Util::base32_decode(pid, std::span<unsigned char>(raw, sizeof(raw)));

	Samurai::Crypto::Digest::Tiger tiger;
	tiger.update(raw, sizeof(raw));

	char out[CID_LENGTH + 1] = { 0, };
	tiger.digest().getFormattedString(
		Samurai::Crypto::Digest::HashValue::Format::Base32, out, sizeof(out));
	return std::string(out);
}

/* What the config file has to say about who we are. */
std::string stored(QuickDC::Preferences* config, const char* key)
{
	config->setGroup("User");
	if (!config->haveSetting(key)) return std::string("(absent)");
	return std::string(config->getString(key));
}

/*
 * The fixture. One Core, whose configuration every identity here is read from.
 */
std::string cfg;
std::string ses;

QuickDC::Preferences* config()
{
	return core->config;
}

/* The identity as generated from nothing, which most cases below start from. */
bool generate(std::string& cid, std::string& pid)
{
	config()->setGroup("User");
	config()->setString("Private ID", "");

	ADC::CID* id = freshIdentity();
	if (!id || !id->getCID() || !id->getPID()) return false;

	cid = id->getCID();
	pid = id->getPID();
	return true;
}

}

EXO_SETUP({
	cfg = scratch("config.conf");
	ses = scratch("session.conf");
	if (!writeFile(cfg, "[User]\nNickname=tester\n")) return 0;

	core = new QuickDC::Core(cfg, ses);
	return 1;
});

EXO_TEARDOWN({
	held_identity.reset();

	/* Before the files, which it holds open and rewrites as it goes. */
	delete core;
	core = 0;

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Generating one                                                            */
/* ------------------------------------------------------------------------- */

/* With nothing stored, an identity is invented and written down. */
EXO_TEST(cid_an_identity_is_generated_when_there_is_none,
{
	ADC::CID* id = freshIdentity();
	EXO_ASSERT_NOT_NULL(id);
	EXO_ASSERT_NOT_NULL(id->getCID());
	EXO_ASSERT_NOT_NULL(id->getPID());

	const std::string cid = id->getCID();
	const std::string pid = id->getPID();

	EXO_ASSERT(cid.size() == CID_LENGTH);
	EXO_ASSERT(pid.size() == CID_LENGTH);
	EXO_ASSERT(ADC::CID::verify(cid.c_str()));
	EXO_ASSERT(ADC::CID::verify(pid.c_str()));
	return 1;
});

/* The CID is the hash of the PID, and nothing else. */
EXO_TEST(cid_the_cid_is_the_hash_of_the_pid,
{
	std::string cid, pid;
	EXO_ASSERT(generate(cid, pid));

	EXO_ASSERT(!pid.empty());
	EXO_ASSERT(cidOf(pid.c_str()) == cid);
	EXO_ASSERT(ADC::CID::verifyCIDandPID(cid.c_str(), pid.c_str()));
	return 1;
});

/* And the pair was written to the configuration. */
EXO_TEST(cid_the_identity_is_written_to_the_configuration,
{
	std::string cid, pid;
	EXO_ASSERT(generate(cid, pid));

	EXO_ASSERT(stored(config(), "Private ID") == pid);
	EXO_ASSERT(stored(config(), "Client ID") == cid);

	/* And to the file, or it is only this run's identity. */
	const std::string raw = readFile(cfg);
	EXO_ASSERT(raw.find("Private ID=" + pid) != std::string::npos);
	EXO_ASSERT(raw.find("Client ID=" + cid) != std::string::npos);
	return 1;
});

/* Starting again finds the same client, which is the whole point. */
EXO_TEST(cid_the_identity_survives_a_restart,
{
	std::string cid, pid;
	EXO_ASSERT(generate(cid, pid));

	ADC::CID* id = freshIdentity();
	EXO_ASSERT_NOT_NULL(id);
	EXO_ASSERT_NOT_NULL(id->getCID());
	EXO_ASSERT(std::string(id->getCID()) == cid);
	EXO_ASSERT_NOT_NULL(id->getPID());
	EXO_ASSERT(std::string(id->getPID()) == pid);
	return 1;
});

/* Whatever was invented last is what is stored and what comes back. */
EXO_TEST(cid_the_identity_settles_down_after_being_replaced,
{
	std::string cid, pid;
	EXO_ASSERT(generate(cid, pid));
	EXO_ASSERT(stored(config(), "Private ID") == pid);

	ADC::CID* again = freshIdentity();
	EXO_ASSERT_NOT_NULL(again);
	EXO_ASSERT_NOT_NULL(again->getPID());
	EXO_ASSERT(std::string(again->getPID()) == pid);
	EXO_ASSERT_NOT_NULL(again->getCID());
	EXO_ASSERT(std::string(again->getCID()) == cid);
	return 1;
});

/* Two identities are not the same identity. */
EXO_TEST(cid_a_generated_identity_is_not_the_same_twice,
{
	std::string seen[8];
	for (size_t n = 0; n < 8; n++)
	{
		std::string cid;
		EXO_ASSERT(generate(cid, seen[n]));
	}

	for (size_t a = 0; a < 8; a++)
	{
		EXO_ASSERT(seen[a].size() == CID_LENGTH);
		for (size_t b = a + 1; b < 8; b++)
		{
			if (seen[a] == seen[b])
				EXO_FAIL("two generated identities are the same");
		}
	}
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What is refused                                                           */
/* ------------------------------------------------------------------------- */

/*
 * A stored PID that is not one is not used: a new pair is generated instead, and
 * the rubbish never becomes this client's secret.
 */
EXO_TEST(cid_a_stored_private_id_that_is_not_one_is_replaced,
{
	static const char* rubbish[] = {
		"",
		"tooshort",
		"999999999999999999999999999999999999999",   /* right length, wrong alphabet */
		"M6CW4G5RICVGE67E4YDA6A3GX2EUZ2YEZA4S64",    /* one short */
		"M6CW4G5RICVGE67E4YDA6A3GX2EUZ2YEZA4S64IA",  /* one long */
		0
	};

	for (size_t n = 0; rubbish[n]; n++)
	{
		config()->setGroup("User");
		config()->setString("Private ID", rubbish[n]);

		ADC::CID* id = freshIdentity();
		if (!id || !id->getCID() || !id->getPID())
		{
			const std::string why =
				std::string("no identity after a stored \"") + rubbish[n] + "\"";
			EXO_FAIL(why.c_str());
		}

		/* A new one, and a consistent one. */
		if (!ADC::CID::verifyCIDandPID(id->getCID(), id->getPID()))
		{
			const std::string why = std::string("the identity after \"")
				+ rubbish[n] + "\" does not hold together";
			EXO_FAIL(why.c_str());
		}

		if (std::string(id->getPID()) == rubbish[n])
		{
			const std::string why = std::string("the stored rubbish \"")
				+ rubbish[n] + "\" was used as a PID";
			EXO_FAIL(why.c_str());
		}
	}
	return 1;
});

/* A CID that is not the hash of the PID it is offered with. */
EXO_TEST(cid_a_cid_that_is_not_the_hash_of_its_pid_is_refused,
{
	std::string cid, pid;
	EXO_ASSERT(generate(cid, pid));

	EXO_ASSERT(!ADC::CID::verifyCIDandPID(pid.c_str(), pid.c_str()));
	EXO_ASSERT(!ADC::CID::verifyCIDandPID(cid.c_str(), cid.c_str()));

	/* One character out is enough. */
	std::string bent = pid;
	bent[0] = (bent[0] == 'A') ? 'B' : 'A';
	EXO_ASSERT(!ADC::CID::verifyCIDandPID(cid.c_str(), bent.c_str()));
	return 1;
});

/* And input that is not a pair at all. */
EXO_TEST(cid_input_that_is_not_a_pair_is_refused,
{
	std::string cid, pid;
	EXO_ASSERT(generate(cid, pid));

	EXO_ASSERT(!ADC::CID::verifyCIDandPID(0, 0));
	EXO_ASSERT(!ADC::CID::verifyCIDandPID(cid.c_str(), 0));
	EXO_ASSERT(!ADC::CID::verifyCIDandPID(0, pid.c_str()));
	EXO_ASSERT(!ADC::CID::verifyCIDandPID("", ""));
	EXO_ASSERT(!ADC::CID::verifyCIDandPID("tooshort", "tooshort"));
	EXO_ASSERT(!ADC::CID::verifyCIDandPID(cid.c_str(),
		"999999999999999999999999999999999999999"));
	return 1;
});

/*
 * Documented rather than endorsed: toString() answers out of one buffer per
 * thread, so the second call on a thread overwrites what the first returned. Every caller today uses the answer before calling again; this is here
 * so that stops being an accident.
 */
EXO_TEST(cid_a_sid_is_rendered_into_one_shared_buffer,
{
	const char* first = ADC::SID::toString(1);
	EXO_ASSERT(std::string(first) == "AAAB");

	const char* second = ADC::SID::toString(2);
	EXO_ASSERT(std::string(second) == "AAAC");

	/* The same storage, so the first pointer now reads as the second. */
	EXO_ASSERT_EQ_PTR(first, second);
	return 1;
});
