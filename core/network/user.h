/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_USERINFO_H
#define HAVE_QUICKDC_USERINFO_H

#include "quickdc.h"

namespace QuickDC {

class User {

	public:
		enum UserAccess { Invalid, Regular, Bot, Registered, Operator };
	
		User(const char* id, const char* nick, const char* description, const char* email, uint64_t sharedBytes);
		User(const char* id);
		// User(const User& user);
		virtual ~User();

		mutable void* ptr_data;

	public:
		bool           setID(const char*);
		const char*    getID() const;
		bool           setNick(const char*);
		const char*    getNick() const;
		bool           setDescription(const char*);
		const char*    getDescription() const;
		bool           setEmail(const char*);
		const char*    getEmail() const;
		bool           setSharedBytes(uint64_t);
		uint64_t       getSharedBytes() const;
		bool           setSpeed(const char*);
		const char*    getSpeed() const;
		bool           setUserAgent(const char*);
		const char*    getUserAgent() const;
		bool           setClientID(uint8_t*);
		const uint8_t* getClientID() const;
		bool           setPrivateID(uint8_t*);
		const uint8_t* getPrivateID() const;
		bool           setSessionID(uint32_t);
		uint32_t       getSessionID() const;
		bool           setNumSlots(uint32_t);
		uint32_t       getNumSlots() const;
		bool           setSharedFiles(uint32_t);
		uint32_t       getSharedFiles() const;
		bool           setHubsOperator(uint32_t);
		uint32_t       getHubsOperator() const;
		bool           setHubsRegistered(uint32_t);
		uint32_t       getHubsRegistered() const;
		bool           setHubsRegular(uint32_t);
		uint32_t       getHubsRegular() const;
		bool           setMaxSpeedUpload(uint32_t);
		uint32_t       getMaxSpeedUpload() const;
		bool           setMaxSpeedDownload(uint32_t);
		uint32_t       getMaxSpeedDownload() const;
		bool           setAutoSlotSpeed(uint32_t);
		uint32_t       getAutoSlotSpeed() const;
		bool           setAutoSlotLimit(uint32_t);
		uint32_t       getAutoSlotLimit() const;
		bool           setPortIPv4(uint16_t);
		uint16_t       getPortIPv4() const;
		bool           setPortIPv6(uint16_t);
		uint16_t       getPortIPv6() const;
		bool           setAddressIPv4(const char*);
		const char*    getAddressIPv4() const;
		bool           setAddressIPv6(const char*);
		const char*    getAddressIPv6() const;

		
		bool           isOp() const;
		bool           setOp();

	public:
		void dump();

	private:
		void clean();

	protected:
		char            nick[64];
		char            id[64];
		uint64_t        shared;
		UserAccess      access;
		char            description[256];
		char            email[128];
		char            speed[16];
		char            userAgent[64];
		
		uint8_t         clientID[64];
		uint8_t         privateID[64];
		uint32_t        sessionID;
		uint32_t        numSlots;
		uint32_t        sharedFiles;
		uint32_t        hubsOperator;
		uint32_t        hubsRegistered;
		uint32_t        hubsRegular;
		uint32_t        maxSpeedUpload;
		uint32_t        maxSpeedDownload;
		uint32_t        autoSlotSpeed;
		uint32_t        autoSlotLimit;
		
		char            address_ipv4[16];
		char            address_ipv6[40];
		uint16_t        port_ipv4;
		uint16_t        port_ipv6;
		

		bool            away;     /* user is away */
		bool            fast;     /* fast upload flag */
		bool            reliable; /* server flag */
		
	private:
		User();
		
	friend class UserManager;
};

}

#endif // HAVE_QUICKDC_USERINFO_H
