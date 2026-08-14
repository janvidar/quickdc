/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HUB_MANAGER_H
#define HAVE_QUICKDC_HUB_MANAGER_H

#include <samurai/timer.h>

#include <chrono>
#include <vector>

namespace QuickDC {

class Socket;
class Hub;
class HubSession;

namespace Share {
	class SearchRequest;
}

/**
 * How long to wait before telling the hubs that our own status changed.
 *
 * Every request inside the window collapses into one update at the end of it,
 * and the window restarts on nothing - so a client reconnecting to six hubs,
 * which changes the hub counts on each attempt, sends one update rather than
 * six to every hub it is on. Hubs treat a client that updates constantly as
 * flooding, and none of what changes here is worth a round trip the moment it
 * happens.
 *
 * The delay is deliberately at the end rather than the start: an update sent
 * immediately would carry the counts from the middle of the reconnect, and then
 * be suppressed for the window that follows, leaving every hub holding a number
 * that was never true for long.
 */
#define QUICKDC_HUB_INFO_UPDATE_DELAY_SECONDS 10

/**
 * This class manages multiple hub connections.
 */
class HubManager : public Samurai::TimerListener {
	public:
		HubManager();
		~HubManager() override;
	
		void add(Hub* hub);
		void remove(Hub* hub);
		
		/**
		 * Returns a hub for a given user ID.
		 * @param id is either nickname (NMDC) or sid (ADC).
		 */
		Hub* lookupUser(const char* id);

		/**
		 * Returns a hub where the given client ID is online, or 0.
		 */
		Hub* lookupUserByClientID(const char* cid);

		/**
		 * Returns the hub we are logged in to at the given URL, or 0.
		 * The URL must be in the canonical form returned by Hub::getUrl(),
		 * which is what a FileSource stores.
		 */
		Hub* getHubByUrl(const char* url);

		/** Returns a hub where the given nickname is online, or 0. */
		Hub* lookupUserByNick(const char* nick);

		/** Send a search to every hub we are logged in to. */
		size_t searchAllHubs(Share::SearchRequest* request);

		/**
		 * Send a search to one hub and to no other.
		 *
		 * For something that is only any hub's business because it was learned there.
		 * A bulletin board's posts are the case: the hash of a post is on that hub's
		 * board, where every subscriber can read it, and asking a second hub about it
		 * tells that hub's operator which boards this client reads elsewhere - which
		 * is a disclosure nothing about the download requires.
		 *
		 * @param url canonical, as Hub::getUrl() returns it and a FileSource stores it.
		 * @return 1 where it went out, and 0 for a hub this client is not logged in to.
		 */
		size_t searchOneHub(const char* url, Share::SearchRequest* request);

		/**
		 * Ask for our own status to be re-sent to every hub we are logged in to.
		 *
		 * For something a hub was told once and would otherwise keep believing -
		 * the port a peer should use, and how many hubs we are on as guest,
		 * registered user and operator. A hub that holds a stale port hands every
		 * other client an address with nothing behind it.
		 *
		 * Throttled: this schedules one update rather than sending anything, and
		 * any number of calls inside the window produce a single update. Call it
		 * freely.
		 */
		void requestClientInfoUpdate();

		/** True while an update is scheduled but has not been sent. */
		bool isClientInfoUpdatePending() const { return info_timer != 0; }

		/**
		 * How long a request waits before the update goes out.
		 *
		 * Settable because how much updating a hub tolerates is a property of the
		 * hub rather than of this client. Takes effect on the next request; one
		 * already scheduled keeps the window it was given.
		 */
		void setClientInfoUpdateDelay(std::chrono::milliseconds delay)
		{ info_delay = delay; }

		std::chrono::milliseconds getClientInfoUpdateDelay() const
		{ return info_delay; }

		/**
		 * Send it now, and cancel anything scheduled.
		 *
		 * The unthrottled path, for a caller that knows it is about to stop
		 * running an event loop. Prefer requestClientInfoUpdate().
		 *
		 * @return how many hubs were told.
		 */
		size_t sendClientInfoToAllHubs();

		/**
		 * Returns the number of hubs where we are logged in
		 * as normal users.
		 *
		 * 'except' leaves one session out of the tally, for a caller that is
		 * about to account for itself: a session announcing these counts is
		 * telling a hub how many hubs it is on including that one, and cannot
		 * read its own contribution here because it is not logged in until the
		 * announcement it is composing has been answered.
		 */
		size_t countHubsNormal(const HubSession* except = 0);

		/**
		 * Returns the number of hubs where we are logged in
		 * as registered users.
		 */
		size_t countHubsRegistered(const HubSession* except = 0);

		/**
		 * Returns the number of hubs where we are logged in
		 * as operators.
		 */
		size_t countHubsOperator(const HubSession* except = 0);

	protected:
		void EventTimeout(Samurai::Timer* timer) override;

		/**
		 * Give the search a token, and remember it, so a reply can be told from a
		 * stranger by what it answers. Shared by both ways of sending one.
		 */
		void rememberSearchToken(Share::SearchRequest* request);

		std::vector<Hub*> hubs;

		/** Non-null while an update is scheduled. */
		Samurai::Timer* info_timer;
		std::chrono::milliseconds info_delay;
};

}

#endif // HAVE_QUICKDC_HUB_MANAGER_H
