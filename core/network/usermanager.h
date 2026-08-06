/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_USERMANAGER_H
#define HAVE_QUICKDC_USERMANAGER_H

#include <map>
#include <set>
#include <string>
#include <vector>
#include "quickdc.h"

namespace QuickDC {

class User;

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

		UserManager(const UserManager&) = delete;
		UserManager& operator=(const UserManager&) = delete;

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
		 * Also slow, and for the same reason: the index is by session ID.
		 *
		 * On ADC the client ID is what identifies a user across sessions, so
		 * it is what the download queue stores in a FileSource - the session ID
		 * is only good for as long as the user stays on the hub. NMDC users
		 * have no client ID, so this finds nothing there.
		 */
		const User* getUserByClientID(const char* cid);

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
		 * The hub's operators, all of them.
		 *
		 * NMDC names them in an $OpList of its own rather than in each user's
		 * info, and resends the list entire whenever it changes - so this is the
		 * answer about everybody at once: a name in it is an operator and a name
		 * absent from it is not.
		 *
		 * Remembered as well as applied, because the list can arrive before the
		 * users named in it do - their info is asked for and answered as separate
		 * commands, and the replies may land afterwards. info() consults what was
		 * remembered, so somebody arriving late is still marked.
		 */
		void setOperators(const std::vector<std::string>& nicks);
		
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

		/**
		 * Who setOperators() last named, and whether it has ever been called.
		 *
		 * Kept apart because an empty list and no list are different answers: a
		 * hub with no operators says so, while ADC never says anything here at
		 * all - it carries the same fact in each user's own info, and a user
		 * marked from there must not be unmarked for missing a list nobody sent.
		 */
		std::set<std::string> operators;
		bool haveOperatorList = false;

		UserEventHandler* event;
};




}

#endif // HAVE_QUICKDC_USERMANAGER_H
