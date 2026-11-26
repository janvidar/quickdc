/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <stdio.h>
#include <string.h>
#include "quickdc.h"
#include "network/adc/cid.h"
#include <samurai/crypto/digest/tiger.h>
#include "config/preferences.h"
#include "api/core.h"
#include "config/settings.h"
#include <samurai/timestamp.h>
#include <samurai/util/base32.h>
#include <span>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <samurai/posix.h>
#include <string>

#ifdef WIN32
#include <samurai/io/net/socketglue.h>
#endif

#define HASH 40
static const char* ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

const char* ADC::SID::toString(sid_t sid_) {
	thread_local char t_sid[5];
	sid_t sid = (sid_ & 0xFFFFF); // 20 bits;
	sid_t A, B, C, D = 0;
	D     = (sid % 32);
	sid   = (sid - D) / 32;
	C     = (sid % 32);
	sid   = (sid - C) / 32;
	B     = (sid % 32);
	sid   = (sid - B) / 32;
	A     = (sid % 32);
	t_sid[0] = ALPHABET[A];
	t_sid[1] = ALPHABET[B];
	t_sid[2] = ALPHABET[C];
	t_sid[3] = ALPHABET[D];
	t_sid[4] = 0;
	return t_sid;
}

sid_t ADC::SID::fromString(const char* sid) {
	if (!sid) return 0;

	const std::string_view text(sid);
	if (text.size() != 4) return 0;

	static const sid_t factors[] = { 32768, 1024, 32, 1 };
	sid_t nsid = 0;
	for (size_t n = 0; n < 4; n++) {
		const size_t x = std::string_view(ALPHABET).find(text[n]);
		if (x == std::string_view::npos) return 0;
		nsid += static_cast<sid_t>(x) * factors[n];
	}
	return nsid;
}

bool ADC::CID::verify(const char* cid) {
	if (!cid) return false;

	const std::string_view id(cid);
	if (id.size() != (HASH-1)) return false;
	return id.find_first_not_of(ALPHABET) == std::string_view::npos;
}

bool ADC::CID::verifyCIDandPID(const char* cid, const char* pid) {
	if (!verify(cid) || !verify(pid)) return false;
	
	char x_cid[HASH];
	uint8_t raw_pid[Samurai::Crypto::Digest::TIGER_HASH_SIZE];
	
	Samurai::Util::base32_decode(pid, std::span<unsigned char>(raw_pid, Samurai::Crypto::Digest::TIGER_HASH_SIZE));
	
	Samurai::Crypto::Digest::Tiger tiger;
	tiger.update(raw_pid, Samurai::Crypto::Digest::TIGER_HASH_SIZE);
	tiger.digest().getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, x_cid, HASH);
	
	return Samurai::Util::iequals(x_cid, cid);
}

ADC::CID::CID(QuickDC::Preferences& config_) : config(config_) {
	if (!load()) {
		generate();
		store();
	}
}

ADC::CID::~CID() {
}

/*
 * The PID is this client's long term secret: the CID everyone sees is its hash,
 * so anyone who can reproduce the PID becomes us on every hub. It used to be
 * derived from the hostname, a one-second timestamp, the build id and getpid(),
 * none of which are secret - the build id is even published in our own INF - and
 * together they leave far too small a space to search. It comes from the system
 * CSPRNG instead, and the PID is no longer written to the log.
 */
void ADC::CID::generate() {
	QDBG("Generating CID...");

	uint8_t raw_pid[Samurai::Crypto::Digest::TIGER_HASH_SIZE];
	if (RAND_bytes(raw_pid, sizeof(raw_pid)) != 1)
	{
		/* A guessable identity is worse than refusing to invent one. */
		QERR("No secure randomness available to generate a client identity");
		cid.clear();
		pid.clear();
		return;
	}

	/* Both encoders write into a plain buffer, and HASH is what either of them
	   needs for 24 bytes of hash, terminator included. */
	char pid_encoded[HASH] = { 0, };
	char cid_encoded[HASH] = { 0, };

	Samurai::Util::base32_encode(std::span<const unsigned char>(raw_pid, sizeof(raw_pid)), std::span<char>(pid_encoded, HASH));

	Samurai::Crypto::Digest::Tiger tiger;
	tiger.update(raw_pid, sizeof(raw_pid));
	tiger.digest().getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, cid_encoded, HASH);

	pid = pid_encoded;
	cid = cid_encoded;

	/* The encoded private ID is the secret in the form it is stored in, so the
	   scratch copy goes the same way as the raw bytes. */
	OPENSSL_cleanse(raw_pid, sizeof(raw_pid));
	OPENSSL_cleanse(pid_encoded, sizeof(pid_encoded));

	QDBG("CID generated: %s", cid.c_str());
}


bool ADC::CID::load() {
	QDBG("Loading CID from configuration file...");
	
	/*
	 * Nothing stored is the ordinary case on a first run and says nothing worth
	 * reporting. Something stored that is not the length of a CID does: it is an
	 * identity that is about to be replaced.
	 */
	const std::string stored = QuickDC::Settings::read(config, QuickDC::Settings::PrivateId);
	if (stored.size() != static_cast<size_t>(HASH - 1)) {
		if (!stored.empty()) QERR("The stored private ID is not a valid CID; generating a new identity");
		return false;
	}

	pid = stored;

	if (!verify(pid.c_str())) {
		pid.clear();
		return false;
	}

	uint8_t pid_decoded[Samurai::Crypto::Digest::TIGER_HASH_SIZE] = { 0, };
	Samurai::Util::base32_decode(pid, std::span<unsigned char>(pid_decoded, Samurai::Crypto::Digest::TIGER_HASH_SIZE));

	char cid_encoded[HASH] = { 0, };
	Samurai::Crypto::Digest::Tiger tiger;
	tiger.update(pid_decoded, Samurai::Crypto::Digest::TIGER_HASH_SIZE);
	tiger.digest().getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, cid_encoded, HASH);
	cid = cid_encoded;

	/* The CID is public; the PID is the secret behind it and is never logged. */
	QDBG("CID loaded: %s", cid.c_str());
	store();
	return true;
}

bool ADC::CID::store() {
	if (cid.empty() || pid.empty()) return false;
	QDBG("Storing the identity for CID %s", cid.c_str());

	if (verifyCIDandPID(cid.c_str(), pid.c_str())) {
		QuickDC::Settings::write(config, QuickDC::Settings::PrivateId, pid);
		QuickDC::Settings::write(config, QuickDC::Settings::ClientId, cid);
		config.write();
		return true;
	}
	return false;
}

