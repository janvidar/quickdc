/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
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

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define CID_LENGTH 39

static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_cid_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

static std::string readFile(const std::string& path)
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

static bool writeFile(const std::string& path, const std::string& content)
{
	FILE* fh = fopen(path.c_str(), "wb");
	if (!fh) return false;
	const bool ok = fwrite(content.data(), 1, content.size(), fh) == content.size();
	fclose(fh);
	return ok;
}

/* Throws the singleton away, so the next getInstance() starts over. */
static ADC::CID* freshIdentity()
{
	delete ADC::global_cid;
	ADC::global_cid = 0;
	return ADC::CID::getInstance();
}

/*
 * The CID a PID implies, worked out here rather than asked for: the Tiger hash
 * of the decoded private ID, in base32.
 */
static std::string cidOf(const char* pid)
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
static std::string stored(QuickDC::Preferences* config, const char* key)
{
	config->setGroup("User");
	if (!config->haveSetting(key)) return std::string("(absent)");
	return std::string(config->getString(key));
}

int main()
{
	setbuf(stdout, 0);

	const std::string cfg = scratch("config.conf");
	const std::string ses = scratch("session.conf");
	writeFile(cfg, "[User]\nNickname=tester\n");

	/* One core, because it is a singleton and the identity reads its config. */
	QuickDC::Core core(cfg, ses);
	QuickDC::Preferences* config = core.config;

	std::string firstCid;
	std::string firstPid;

	/* With nothing stored, an identity is invented and written down. */
	{
		ADC::CID* id = freshIdentity();
		CHECK(id);
		CHECK(id && id->getCID());
		CHECK(id && id->getPID());

		if (id && id->getCID() && id->getPID())
		{
			firstCid = id->getCID();
			firstPid = id->getPID();
		}

		CHECK(firstCid.size() == CID_LENGTH);
		CHECK(firstPid.size() == CID_LENGTH);
		CHECK(ADC::CID::verify(firstCid.c_str()));
		CHECK(ADC::CID::verify(firstPid.c_str()));
		printf("  ok an identity is generated when there is none\n");
	}

	/* The CID is the hash of the PID, and nothing else. */
	{
		CHECK(!firstPid.empty());
		CHECK(cidOf(firstPid.c_str()) == firstCid);
		CHECK(ADC::CID::verifyCIDandPID(firstCid.c_str(), firstPid.c_str()));
		printf("  ok the CID is the hash of the PID\n");
	}

	/* And the pair was written to the configuration. */
	{
		CHECK(stored(config, "Private ID") == firstPid);
		CHECK(stored(config, "Client ID") == firstCid);

		/* And to the file, or it is only this run's identity. */
		const std::string raw = readFile(cfg);
		CHECK(raw.find("Private ID=" + firstPid) != std::string::npos);
		CHECK(raw.find("Client ID=" + firstCid) != std::string::npos);
		printf("  ok the identity is written to the configuration\n");
	}

	/* Starting again finds the same client, which is the whole point. */
	{
		ADC::CID* id = freshIdentity();
		CHECK(id && id->getCID() && std::string(id->getCID()) == firstCid);
		CHECK(id && id->getPID() && std::string(id->getPID()) == firstPid);
		printf("  ok the identity survives a restart\n");
	}

	/* A stored PID that is not one is not used. */
	{
		const char* rubbish[] = {
			"",
			"tooshort",
			"999999999999999999999999999999999999999",   /* right length, wrong alphabet */
			"M6CW4G5RICVGE67E4YDA6A3GX2EUZ2YEZA4S64",    /* one short */
			"M6CW4G5RICVGE67E4YDA6A3GX2EUZ2YEZA4S64IA",  /* one long */
			0
		};

		for (size_t n = 0; rubbish[n]; n++)
		{
			config->setGroup("User");
			config->setString("Private ID", rubbish[n]);

			ADC::CID* id = freshIdentity();
			if (!id || !id->getCID() || !id->getPID())
			{
				printf("FAIL %s:%d no identity after a stored \"%s\"\n",
					__FILE__, __LINE__, rubbish[n]);
				failures++;
				continue;
			}

			/* A new one, and a consistent one. */
			if (!ADC::CID::verifyCIDandPID(id->getCID(), id->getPID()))
			{
				printf("FAIL %s:%d the identity after \"%s\" does not hold together\n",
					__FILE__, __LINE__, rubbish[n]);
				failures++;
			}

			if (std::string(id->getPID()) == rubbish[n])
			{
				printf("FAIL %s:%d the stored rubbish \"%s\" was used as a PID\n",
					__FILE__, __LINE__, rubbish[n]);
				failures++;
			}
		}
		printf("  ok a stored private ID that is not one is replaced\n");
	}

	/* Whatever was invented last is what is stored and what comes back. */
	{
		ADC::CID* id = freshIdentity();
		CHECK(id && id->getPID());

		const std::string pid = id->getPID();
		const std::string cid = id->getCID();
		CHECK(stored(config, "Private ID") == pid);

		ADC::CID* again = freshIdentity();
		CHECK(again && again->getPID() && std::string(again->getPID()) == pid);
		CHECK(again && again->getCID() && std::string(again->getCID()) == cid);
		printf("  ok the identity settles down after being replaced\n");
	}

	/* Two identities are not the same identity. */
	{
		std::string seen[8];
		for (size_t n = 0; n < 8; n++)
		{
			config->setGroup("User");
			config->setString("Private ID", "");

			ADC::CID* id = freshIdentity();
			CHECK(id && id->getPID());
			if (id && id->getPID()) seen[n] = id->getPID();
		}

		for (size_t a = 0; a < 8; a++)
		{
			CHECK(seen[a].size() == CID_LENGTH);
			for (size_t b = a + 1; b < 8; b++)
			{
				if (seen[a] == seen[b])
				{
					printf("FAIL %s:%d two generated identities are the same\n",
						__FILE__, __LINE__);
					failures++;
				}
			}
		}
		printf("  ok a generated identity is not the same twice\n");
	}

	/* A CID that is not the hash of the PID it is offered with. */
	{
		CHECK(!ADC::CID::verifyCIDandPID(firstPid.c_str(), firstPid.c_str()));
		CHECK(!ADC::CID::verifyCIDandPID(firstCid.c_str(), firstCid.c_str()));

		/* One character out is enough. */
		std::string bent = firstPid;
		bent[0] = (bent[0] == 'A') ? 'B' : 'A';
		CHECK(!ADC::CID::verifyCIDandPID(firstCid.c_str(), bent.c_str()));
		printf("  ok a CID that is not the hash of its PID is refused\n");
	}

	/* And input that is not a pair at all. */
	{
		CHECK(!ADC::CID::verifyCIDandPID(0, 0));
		CHECK(!ADC::CID::verifyCIDandPID(firstCid.c_str(), 0));
		CHECK(!ADC::CID::verifyCIDandPID(0, firstPid.c_str()));
		CHECK(!ADC::CID::verifyCIDandPID("", ""));
		CHECK(!ADC::CID::verifyCIDandPID("tooshort", "tooshort"));
		CHECK(!ADC::CID::verifyCIDandPID(firstCid.c_str(), "999999999999999999999999999999999999999"));
		printf("  ok input that is not a pair is refused\n");
	}

	/*
	 * Documented rather than endorsed: toString() answers out of one buffer it
	 * keeps for the purpose, so the second call in an expression overwrites what
	 * the first returned. Every caller today uses the answer before calling
	 * again; this is here so that stops being an accident.
	 */
	{
		const char* first = ADC::SID::toString(1);
		CHECK(std::string(first) == "AAAB");

		const char* second = ADC::SID::toString(2);
		CHECK(std::string(second) == "AAAC");

		/* The same storage, so the first pointer now reads as the second. */
		CHECK(first == second);
		printf("  ok a SID is rendered into one shared buffer\n");
	}

	delete ADC::global_cid;
	ADC::global_cid = 0;

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
