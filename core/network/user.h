/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_USERINFO_H
#define HAVE_QUICKDC_USERINFO_H

#include "quickdc.h"

#include <string>

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

		/**
		 * The keyprint of the certificate this user presents on ADCS peer
		 * connections, as announced in the KP field of their INF. Empty if
		 * they published none.
		 */
		bool           setKeyprint(const char*);
		const char*    getKeyprint() const;

		/**
		 * Whether this user published a keyprint at all.
		 *
		 * Asked before the value is used, because a peer connection is only
		 * held to a keyprint when there is one to hold it to - and a user
		 * without one is the ordinary case, not a failure.
		 */
		bool           hasKeyprint() const;

		/**
		 * The features this user announced in the SU field of their INF, as
		 * written - a comma separated set.
		 */
		bool           setSupports(const char*);
		const char*    getSupports() const;

		/**
		 * Whether that set names a feature.
		 *
		 * Matched whole rather than as a substring, because the flags share their
		 * letters: a peer announcing ADC0 must not be read as announcing DC0, and
		 * one announcing nothing must not be credited from a neighbouring field.
		 */
		bool           supports(const char* feature) const;

		
		bool           isOp() const;

		/**
		 * Whether the hub grants this user operator rights.
		 *
		 * Takes the answer rather than only the promotion, because both protocols
		 * can withdraw it while the user stays where they are: an ADC INF arrives
		 * with a new CT, and an NMDC $OpList is resent whole with the name left
		 * out.
		 */
		bool           setOp(bool);

		/**
		 * Whether the user says they are away.
		 *
		 * Both protocols carry it: ADC in the AW field of an INF, NMDC in the
		 * status byte on the end of the speed field of a $MyINFO. Neither
		 * distinguishes being away from being away and not reading the chat -
		 * ADC's "extended away" - so both arrive here as away.
		 *
		 * A hub that says nothing about it leaves this false, which is the
		 * ordinary case rather than a claim to be present.
		 */
		bool           isAway() const;
		bool           setAway(bool);

	public:
		void dump();

	private:
		void clean();

	protected:
		/*
		 * Whatever a hub or a peer sent, each held to the length its setter
		 * allows. A field nobody has told us about is empty; no empty value
		 * means anything of its own, so the two cases do not need telling apart.
		 */
		std::string     nick;
		std::string     id;
		uint64_t        shared;
		UserAccess      access;
		std::string     description;
		std::string     email;
		std::string     speed;
		std::string     userAgent;

		/* Base32 on the wire, and compared as strings everywhere else. */
		std::string     clientID;
		std::string     privateID;
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
		
		std::string     address_ipv4;
		std::string     address_ipv6;
		std::string     keyprint;
		std::string     supported;
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
