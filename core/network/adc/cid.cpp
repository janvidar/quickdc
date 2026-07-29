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
#include <samurai/timestamp.h>
#include <samurai/util/base32.h>
#include <span>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <unistd.h>
#include <string>

#ifdef WIN32
#include <samurai/io/net/socketglue.h>
#endif

#define HASH 40
static const char* ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

const char* ADC::SID::toString(sid_t sid_) {
	static char t_sid[5];
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
	if (!sid || strlen(sid) != 4) return 0;
	sid_t nsid = 0;
	sid_t n, x;
	sid_t factors[] = { 32768, 1024, 32, 1};
	for (n = 0; n < 4; n++) {
		for (x = 0; x < strlen(ALPHABET); x++)
			if (sid[n] == ALPHABET[x]) break;
		if (x == 32) return 0;
		nsid += x * factors[n];
	}
	return nsid;
}

bool ADC::CID::verify(const char* cid) {
	if (!cid || strlen(cid) != (HASH-1)) return false;
	for (size_t n = 0; n < strlen(cid); n++) {
		bool hit = false;
		for (size_t x = 0; x < strlen(ALPHABET); x++) {
			if (cid[n] == ALPHABET[x]) hit = true;
		}
		if (!hit) return false;
	}
	return true;
}

bool ADC::CID::verifyCIDandPID(const char* cid, const char* pid) {
	if (!verify(cid) || !verify(pid)) return false;
	
	char x_cid[HASH];
	uint8_t raw_pid[Samurai::Crypto::Digest::TIGER_HASH_SIZE];
	
	Samurai::Util::base32_decode(pid, std::span<unsigned char>((unsigned char*) raw_pid, Samurai::Crypto::Digest::TIGER_HASH_SIZE));
	
	Samurai::Crypto::Digest::Tiger tiger;
	Samurai::Crypto::Digest::HashValue* value;
	
	tiger.update(raw_pid, Samurai::Crypto::Digest::TIGER_HASH_SIZE);
	value = tiger.digest();
	value->getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, x_cid, HASH);
	
	return (strcasecmp(x_cid, cid) == 0);
}

ADC::CID* ADC::global_cid = 0;

ADC::CID* ADC::CID::getInstance() {
	if (!global_cid) {
		global_cid = new CID();
	}
	return global_cid;
}

ADC::CID::CID() : cid(0), pid(0) {
	if (!load()) {
		generate();
		store();
	}
}

ADC::CID::~CID() {
	delete[] cid;
	delete[] pid;
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
		cid = 0;
		pid = 0;
		return;
	}

	cid = new char[HASH];
	pid = new char[HASH];

	Samurai::Util::base32_encode(std::span<const unsigned char>(raw_pid, sizeof(raw_pid)), std::span<char>(pid, HASH));

	Samurai::Crypto::Digest::Tiger tiger;
	tiger.update(raw_pid, sizeof(raw_pid));
	Samurai::Crypto::Digest::HashValue* value = tiger.digest();
	value->getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, cid, HASH);

	OPENSSL_cleanse(raw_pid, sizeof(raw_pid));

	QDBG("CID generated: %s", cid);
}


bool ADC::CID::load() {
	QDBG("Loading CID from configuration file...");
	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("User");
	
	/*
	 * Length checked before copying, rather than strncpy'ing HASH bytes into the
	 * buffer and hoping: a stored value that long leaves no terminator behind,
	 * and verify() then takes strlen() of it past the end of the allocation.
	 */
	const char* stored = config->getString("Private ID", "");
	if (strlen(stored) != (size_t) (HASH - 1)) {
		if (*stored) QERR("The stored private ID is not a valid CID; generating a new identity");
		return false;
	}

	pid = new char[HASH];
	cid = new char[HASH];
	memcpy(pid, stored, HASH);

	if (!verify(pid)) {
		delete[] pid; pid = 0;
		delete[] cid; cid = 0;
		return false;
	}

	

	uint8_t pid_decoded[Samurai::Crypto::Digest::TIGER_HASH_SIZE] = { 0, };
	Samurai::Util::base32_decode(pid, std::span<unsigned char>((unsigned char*) pid_decoded, Samurai::Crypto::Digest::TIGER_HASH_SIZE));
	
	Samurai::Crypto::Digest::Tiger tiger;
	Samurai::Crypto::Digest::HashValue* value;
	tiger.update(pid_decoded, Samurai::Crypto::Digest::TIGER_HASH_SIZE);
	value = tiger.digest();
	value->getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, cid, HASH);
	
	/* The CID is public; the PID is the secret behind it and is never logged. */
	QDBG("CID loaded: %s", cid);
	store();
	return true;
}

bool ADC::CID::store() {
	if (!cid || !pid) return false;
	QDBG("Storing the identity for CID %s", cid);

	if (verifyCIDandPID(cid, pid)) {
		QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
		config->setGroup("User");
		config->setString("Private ID", pid);
		config->setString("Client ID", cid);
		config->write();
		return true;
	}
	return false;
}

