/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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
	memset(id,          0,  64);
	memset(nick,        0,  64);
	memset(description, 0, 256);
	memset(email,       0, 128);
	memset(speed,       0,  16);
	memset(userAgent,   0,  64);
	memset(clientID,    0,  64);
	memset(privateID,   0,  64);
	memset(address_ipv4,0,  16);
	memset(address_ipv6,0,  40);
	
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

const char* QuickDC::User::getID() const               { return id; }
const char* QuickDC::User::getNick() const             { return nick; }
const char* QuickDC::User::getDescription() const      { return description; }
const char* QuickDC::User::getEmail() const            { return email; }
const char* QuickDC::User::getSpeed() const            { return speed; }
const char* QuickDC::User::getUserAgent() const        { return userAgent; }
uint32_t    QuickDC::User::getSessionID() const        { return sessionID; }
const uint8_t*    QuickDC::User::getPrivateID() const  { return privateID; }
const uint8_t*    QuickDC::User::getClientID() const   { return clientID; }
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
const char* QuickDC::User::getAddressIPv4() const      { return address_ipv4; }
const char* QuickDC::User::getAddressIPv6() const      { return address_ipv6; }
uint16_t    QuickDC::User::getPortIPv4() const         { return port_ipv4; }
uint16_t    QuickDC::User::getPortIPv6() const         { return port_ipv6; }


#define COPY_STR_IF_CHANGED(str, old, max) { \
	if (str && strcmp(old, str) == 0) return false; \
	if (str) strncpy(old, str, max); \
	else memset(old, 0, max); \
	return true; \
}

#define COPY_NUM_IF_CHANGED(num, old) { \
	if (num == old) return false; \
	old = num; \
	return true; \
}

#define COPY_MEM_IF_CHANGED(mem, old, max) { \
	if (mem && memcmp(old, mem, max) == 0) return false; \
	if (mem) memcpy(old, mem, max); \
	else memset(old, 0, max); \
	return true; \
}

bool        QuickDC::User::setID(const char* str)          COPY_STR_IF_CHANGED(str, id, 64);
bool        QuickDC::User::setNick(const char* str)        COPY_STR_IF_CHANGED(str, nick, 64);
bool        QuickDC::User::setDescription(const char* str) COPY_STR_IF_CHANGED(str, description, 256);
bool        QuickDC::User::setEmail(const char* str)       COPY_STR_IF_CHANGED(str, email, 128);
bool        QuickDC::User::setSpeed(const char* str)       COPY_STR_IF_CHANGED(str, speed, 16);
bool        QuickDC::User::setUserAgent(const char* str)   COPY_STR_IF_CHANGED(str, userAgent, 64);
bool        QuickDC::User::setClientID(uint8_t* mem)       COPY_MEM_IF_CHANGED(mem, clientID, 40);
bool        QuickDC::User::setPrivateID(uint8_t* mem)      COPY_MEM_IF_CHANGED(mem, privateID, 40);
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

bool        QuickDC::User::setAddressIPv4(const char* str) COPY_STR_IF_CHANGED(str, address_ipv4, 16);
bool        QuickDC::User::setAddressIPv6(const char* str) COPY_STR_IF_CHANGED(str, address_ipv6, 40);
bool        QuickDC::User::setPortIPv4(uint16_t n)         COPY_NUM_IF_CHANGED(n, port_ipv4);
bool        QuickDC::User::setPortIPv6(uint16_t n)         COPY_NUM_IF_CHANGED(n, port_ipv6);

