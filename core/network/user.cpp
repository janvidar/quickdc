/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string_view>
#include "user.h"

QuickDC::User::User(const char* id, const char* nick, const char* description, const char* email, uint64_t sharedSize)
{
	clean();
	setID(id);
	setNick(nick);
	setDescription(description);
	setEmail(email);
	setSharedBytes(sharedSize);
	ptr_data = 0;
}

QuickDC::User::User() {
	clean();
	ptr_data = 0;
}

QuickDC::User::User(const char* id) {
	clean();
	setID(id);
	ptr_data = 0;
}
	
QuickDC::User::~User() {
	
}

void QuickDC::User::clean() {
	id.clear();
	nick.clear();
	description.clear();
	email.clear();
	speed.clear();
	userAgent.clear();
	clientID.clear();
	privateID.clear();
	address_ipv4.clear();
	address_ipv6.clear();
	keyprint.clear();

	sessionID = 0;
	shared = 0;
	numSlots = 0;
	sharedFiles = 0;
	hubsOperator = 0;
	hubsRegistered = 0;
	hubsRegular = 0;
	maxSpeedUpload = 0;
	maxSpeedDownload = 0;
	autoSlotSpeed = 0;
	autoSlotLimit = 0;
	port_ipv4 = 0;
	port_ipv6 = 0;
	access = Invalid;
}

bool QuickDC::User::isOp() const {
	return access == Operator;
}

bool QuickDC::User::setOp() {
	bool change = (access != Operator);
	access = Operator;
	return change;
}

/*
 * A field nobody has told us about reads as the empty string rather than as
 * nothing at all: these are asked for in printf arguments and string
 * comparisons all over the interface and the two session state machines, none
 * of which has anything to say about a user without a description.
 */
const char* QuickDC::User::getID() const               { return id.c_str(); }
const char* QuickDC::User::getNick() const             { return nick.c_str(); }
const char* QuickDC::User::getDescription() const      { return description.c_str(); }
const char* QuickDC::User::getEmail() const            { return email.c_str(); }
const char* QuickDC::User::getSpeed() const            { return speed.c_str(); }
const char* QuickDC::User::getUserAgent() const        { return userAgent.c_str(); }
uint32_t    QuickDC::User::getSessionID() const        { return sessionID; }
const uint8_t*    QuickDC::User::getPrivateID() const  { return reinterpret_cast<const uint8_t*>(privateID.c_str()); }
const uint8_t*    QuickDC::User::getClientID() const   { return reinterpret_cast<const uint8_t*>(clientID.c_str()); }
uint64_t    QuickDC::User::getSharedBytes() const      { return shared; }
uint32_t    QuickDC::User::getNumSlots() const         { return numSlots; }
uint32_t    QuickDC::User::getSharedFiles() const      { return sharedFiles; }
uint32_t    QuickDC::User::getHubsOperator() const     { return hubsOperator; }
uint32_t    QuickDC::User::getHubsRegistered() const   { return hubsRegistered; }
uint32_t    QuickDC::User::getHubsRegular() const      { return hubsRegular; }
uint32_t    QuickDC::User::getMaxSpeedUpload() const   { return maxSpeedUpload; }
uint32_t    QuickDC::User::getMaxSpeedDownload() const { return maxSpeedDownload; }
uint32_t    QuickDC::User::getAutoSlotSpeed() const    { return autoSlotSpeed; }
uint32_t    QuickDC::User::getAutoSlotLimit() const    { return autoSlotLimit; }
const char* QuickDC::User::getAddressIPv4() const      { return address_ipv4.c_str(); }
const char* QuickDC::User::getAddressIPv6() const      { return address_ipv6.c_str(); }
const char* QuickDC::User::getKeyprint() const         { return keyprint.c_str(); }
bool        QuickDC::User::hasKeyprint() const         { return !keyprint.empty(); }
uint16_t    QuickDC::User::getPortIPv4() const         { return port_ipv4; }
uint16_t    QuickDC::User::getPortIPv6() const         { return port_ipv6; }


/*
 * Whether the field ended up different, which is what decides whether the
 * interface is told about this user at all.
 *
 * Every field is filled in from what a hub or a peer sent us, so none of the
 * lengths are ours to assume - and each one is bounded, because nothing on the
 * wire has to be reasonable. Anything longer is kept up to the limit, which is
 * also as far as two values are compared: two over-long ones that agree that far
 * are the same value as far as this user is concerned.
 *
 * A null value is not the same as an unchanged one. A caller passing it is
 * saying the field was absent from what arrived, which drops what was there
 * rather than carrying it over from whatever the user last said.
 */
static bool copyIfChanged(std::string& old, const char* str, size_t limit) {
	if (!str) {
		old.clear();
		return true;
	}

	const std::string_view value = std::string_view(str).substr(0, limit);
	if (old == value) return false;
	old = value;
	return true;
}

#define COPY_NUM_IF_CHANGED(num, old) { \
	if (num == old) return false; \
	old = num; \
	return true; \
}

bool        QuickDC::User::setID(const char* str)          { return copyIfChanged(id, str, 63); }
bool        QuickDC::User::setNick(const char* str)        { return copyIfChanged(nick, str, 63); }
bool        QuickDC::User::setDescription(const char* str) { return copyIfChanged(description, str, 255); }
bool        QuickDC::User::setEmail(const char* str)       { return copyIfChanged(email, str, 127); }
bool        QuickDC::User::setSpeed(const char* str)       { return copyIfChanged(speed, str, 15); }
bool        QuickDC::User::setUserAgent(const char* str)   { return copyIfChanged(userAgent, str, 63); }
bool        QuickDC::User::setClientID(uint8_t* mem)       { return copyIfChanged(clientID, reinterpret_cast<const char*>(mem), 63); }
bool        QuickDC::User::setPrivateID(uint8_t* mem)      { return copyIfChanged(privateID, reinterpret_cast<const char*>(mem), 63); }
bool        QuickDC::User::setSessionID(uint32_t n)        COPY_NUM_IF_CHANGED(n, sessionID);
bool        QuickDC::User::setSharedBytes(uint64_t n)      COPY_NUM_IF_CHANGED(n, shared);
bool        QuickDC::User::setNumSlots(uint32_t n)         COPY_NUM_IF_CHANGED(n, numSlots);
bool        QuickDC::User::setSharedFiles(uint32_t n)      COPY_NUM_IF_CHANGED(n, sharedFiles);
bool        QuickDC::User::setHubsOperator(uint32_t n)     COPY_NUM_IF_CHANGED(n, hubsOperator);
bool        QuickDC::User::setHubsRegistered(uint32_t n)   COPY_NUM_IF_CHANGED(n, hubsRegistered);
bool        QuickDC::User::setHubsRegular(uint32_t n)      COPY_NUM_IF_CHANGED(n, hubsRegular);
bool        QuickDC::User::setMaxSpeedUpload(uint32_t n)   COPY_NUM_IF_CHANGED(n, maxSpeedUpload);
bool        QuickDC::User::setMaxSpeedDownload(uint32_t n) COPY_NUM_IF_CHANGED(n, maxSpeedDownload);
bool        QuickDC::User::setAutoSlotSpeed(uint32_t n)    COPY_NUM_IF_CHANGED(n, autoSlotSpeed);
bool        QuickDC::User::setAutoSlotLimit(uint32_t n)    COPY_NUM_IF_CHANGED(n, autoSlotLimit);

bool        QuickDC::User::setAddressIPv4(const char* str) { return copyIfChanged(address_ipv4, str, 15); }
bool        QuickDC::User::setAddressIPv6(const char* str) { return copyIfChanged(address_ipv6, str, 39); }
bool        QuickDC::User::setPortIPv4(uint16_t n)         COPY_NUM_IF_CHANGED(n, port_ipv4);
bool        QuickDC::User::setPortIPv6(uint16_t n)         COPY_NUM_IF_CHANGED(n, port_ipv6);
bool        QuickDC::User::setKeyprint(const char* str)    { return copyIfChanged(keyprint, str, 63); }
bool        QuickDC::User::setSupports(const char* str)    { return copyIfChanged(supported, str, 255); }
const char* QuickDC::User::getSupports() const             { return supported.c_str(); }

/*
 * One entry of the comma separated set, matched whole.
 *
 * A search of the string would credit a peer announcing "ADC0" with "DC0", and the
 * question this answers decides whether a peer is told to do something it may not
 * be able to do.
 */
bool QuickDC::User::supports(const char* feature) const
{
	if (!feature || !*feature) return false;

	const std::string_view wanted(feature);

	size_t at = 0;
	while (at < supported.size())
	{
		size_t end = supported.find(',', at);
		if (end == std::string::npos) end = supported.size();

		if (std::string_view(supported).substr(at, end - at) == wanted) return true;
		at = end + 1;
	}
	return false;
}

