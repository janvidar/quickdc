/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_USERMANAGER_H
#define HAVE_QUICKDC_USERMANAGER_H

#include <map>
#include <string>
#include <vector>
#include "quickdc.h"

namespace QuickDC {

class User;

/**
 * Compare the strings
 */
struct nick_eq {
	bool operator()(const char* s1, const char* s2) const {
		return strcmp(s1, s2) == 0;
	}
};

class UserEventHandler {
	public:
		virtual ~UserEventHandler() {}
		virtual void EventUserJoin(const char* nick) = 0;

		/**
		 * Note: the user data is deleted immediately after this event!
		 */
		virtual void EventUserQuit(const User* data, const char* msg) = 0;
	
		/**
		 * Update the user info, needed for GUI.
		 */
		virtual void EventUserInfo(const User* data) = 0;

		/**
		 * Cleanup all references to User objects.
		 */
		virtual void EventUsersCleanup() { } /* TODO: Perhaps for future use? */
};

class UserManager {
	public:
		UserManager(UserEventHandler* eventhandler);
		virtual ~UserManager();

		/**
		 * Returns a user based on the given ID.
		 * On NMDC this is the nickname, on ADC this is a 4-byte handle.
		 */
		const User* getUser(const char* id);
		
		/**
		 * Be careful about this function since it is not very fast.
		 * Users are indexed by session ID, for NMDC that is the nickname,
		 * on ADC this is completely different.
		 */
		const User* getUserByNick(const char* nick);

		/**
		 * Add a user with a given ID.
		 * Note the user does not have user data attached at this point.
		 */
		void join(const char* id);
		
		/**
		 * Quit a user.
		 */
		void quit(const char* id, const char* msg = 0);
		
		/**
		 * Update user info.
		 */
		void info(const char* id, User* data, uint64_t oldShare, bool changed);
		
		/**
		 * Mark a user as operator
		 */
		void op(const char* id);
		
		/**
		 * Set IP address for a given user.
		 */
		void ip(const char* id, const char* ip);

		/**
		 * Cleanup user database - Clear all pointers.
		 */
		void cleanup();

		size_t countUsers() const;
		size_t countOperators() const;
		uint64_t countShared() const;

		/* Built-in iterators */
		User* first();
		User* next();

	private:
		std::map<std::string, User*> users;
		std::map<std::string, User*>::iterator userIterator;
		uint64_t shared;
		size_t ops;

		UserEventHandler* event;
};




}

#endif // HAVE_QUICKDC_USERMANAGER_H
