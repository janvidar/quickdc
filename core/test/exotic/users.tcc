/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * UserManager is a hub session's memory of who is on the hub: joins, quits,
 * info updates, and the three ways a user is looked up. The session state
 * machines above it decide when to call these; what they do is here.
 *
 * The id is a nickname on NMDC and a four character SID on ADC, and it is
 * whatever arrived on the wire - so the malformed cases matter. Every id
 * reaches a std::map keyed by std::string.
 */

#include "quickdc.h"
#include "network/usermanager.h"
#include "network/user.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

/* Records what the manager reported, so a case can assert on the events too. */
class Events : public QuickDC::UserEventHandler
{
	public:
		std::vector<std::string> joined;
		std::vector<std::string> quit;
		std::vector<std::string> info;
		int cleanups = 0;

		void EventUserJoin(const char* nick) override
		{ joined.push_back(nick ? nick : "(null)"); }

		void EventUserQuit(const QuickDC::User* data, const char*) override
		{ quit.push_back(data && data->getNick() ? data->getNick() : "(null)"); }

		void EventUserInfo(const QuickDC::User* data) override
		{ info.push_back(data && data->getNick() ? data->getNick() : "(null)"); }

		void EventUsersCleanup() override { cleanups++; }
};

/* A user as an INF or a MyINFO would have produced one. */
static QuickDC::User* make(const char* nick, uint64_t shared = 0, const char* cid = 0)
{
	QuickDC::User* u = new QuickDC::User(nick, nick, "", "", shared);
	if (cid) u->setClientID((uint8_t*) cid);
	return u;
}

/* ---- joining and leaving ---- */
EXO_TEST(users_a_user_joins_and_leaves,
{
		Events events;
		QuickDC::UserManager users(&events);

		users.join("AAAA");
		EXO_ASSERT(users.countUsers() == 1);
		EXO_ASSERT(users.getUser("AAAA") != 0);
		EXO_ASSERT(events.joined.size() == 1);

		users.quit("AAAA");
		EXO_ASSERT(users.countUsers() == 0);
		EXO_ASSERT(users.getUser("AAAA") == 0);
		EXO_ASSERT(events.quit.size() == 1);
	
	return 1;
});

/* The same id twice is one user; a hub may repeat itself. */
EXO_TEST(users_joining_twice_is_still_one_user,
{
		QuickDC::UserManager users(0);
		users.join("AAAA");
		users.join("AAAA");
		EXO_ASSERT(users.countUsers() == 1);
	
	return 1;
});

/* Leaving without having joined must not invent anyone. */
EXO_TEST(users_quitting_without_joining_does_nothing,
{
		Events events;
		QuickDC::UserManager users(&events);
		users.quit("NOBODY");
		EXO_ASSERT(users.countUsers() == 0);
		EXO_ASSERT(events.quit.empty());
	
	return 1;
});

/*
	 * A null id is what a malformed command leaves behind, and every one of
	 * these builds a std::string from it.
	 */
EXO_TEST(users_a_null_id_is_refused_everywhere,
{
		QuickDC::UserManager users(0);
		users.join(0);
		users.quit(0);
		users.ip(0, "127.0.0.1");
		users.info(0, make("x"), 0, true);

		EXO_ASSERT(users.countUsers() == 0);
		EXO_ASSERT(users.getUser(0) == 0);
		EXO_ASSERT(users.getUserByNick(0) == 0);
		EXO_ASSERT(users.getUserByClientID(0) == 0);
		EXO_ASSERT(users.getUserByClientID("") == 0);
	
	return 1;
});

/* ---- info ---- */
EXO_TEST(users_info_about_an_unknown_user_adds_them,
{
		Events events;
		QuickDC::UserManager users(&events);

		users.info("AAAA", make("alice", 100), 0, true);
		EXO_ASSERT(users.countUsers() == 1);
		EXO_ASSERT(events.joined.size() == 1);
		EXO_ASSERT(users.countShared() == 100);

		const QuickDC::User* u = users.getUser("AAAA");
		EXO_ASSERT(u && u->getNick() && !strcmp(u->getNick(), "alice"));
	
	return 1;
});

/*
	 * An update hands back the same object, mutated, with what it used to
	 * share - which is how the running total stays right.
	 */
EXO_TEST(users_an_update_replaces_the_old_share_rather_than_adding_to_it,
{
		QuickDC::UserManager users(0);
		users.info("AAAA", make("alice", 100), 0, true);

		QuickDC::User* u = (QuickDC::User*) users.getUser("AAAA");
		const uint64_t was = u->getSharedBytes();
		u->setSharedBytes(250);
		users.info("AAAA", u, was, true);

		EXO_ASSERT(users.countUsers() == 1);
		EXO_ASSERT(users.countShared() == 250);
	
	return 1;
});

EXO_TEST(users_the_shared_total_follows_who_is_on_the_hub,
{
		QuickDC::UserManager users(0);
		users.info("AAAA", make("alice", 100), 0, true);
		users.info("BBBB", make("bob", 400), 0, true);

		EXO_ASSERT(users.countUsers() == 2);
		EXO_ASSERT(users.countShared() == 500);

		users.quit("AAAA");
		EXO_ASSERT(users.countShared() == 400);
	
	return 1;
});

/* An unchanged update is not reported to the interface. */
EXO_TEST(users_only_a_changed_update_is_reported,
{
		Events events;
		QuickDC::UserManager users(&events);
		users.info("AAAA", make("alice"), 0, true);

		QuickDC::User* u = (QuickDC::User*) users.getUser("AAAA");
		events.info.clear();

		users.info("AAAA", u, u->getSharedBytes(), false);
		EXO_ASSERT(events.info.empty());

		users.info("AAAA", u, u->getSharedBytes(), true);
		EXO_ASSERT(events.info.size() == 1);
	
	return 1;
});

/* ---- lookups ---- */
EXO_TEST(users_users_are_found_by_id_nick_and_client_id,
{
		QuickDC::UserManager users(0);
		users.info("AAAA", make("alice", 0, "CIDALICE"), 0, true);
		users.info("BBBB", make("bob", 0, "CIDBOB"), 0, true);

		EXO_ASSERT(users.getUser("AAAA") != 0);
		EXO_ASSERT(users.getUser("ZZZZ") == 0);

		const QuickDC::User* byNick = users.getUserByNick("bob");
		EXO_ASSERT(byNick && byNick->getNick() && !strcmp(byNick->getNick(), "bob"));
		EXO_ASSERT(users.getUserByNick("nobody") == 0);

		const QuickDC::User* byCid = users.getUserByClientID("CIDALICE");
		EXO_ASSERT(byCid && byCid->getNick() && !strcmp(byCid->getNick(), "alice"));
		EXO_ASSERT(users.getUserByClientID("CIDNOBODY") == 0);
	
	return 1;
});

/*
	 * Looking a user up from inside a walk must not move the walk. The two
	 * lookups above used to share the manager's single iterator with it.
	 */
EXO_TEST(users_a_lookup_during_a_walk_does_not_disturb_the_walk,
{
		QuickDC::UserManager users(0);
		users.info("AAAA", make("alice", 0, "CIDA"), 0, true);
		users.info("BBBB", make("bob", 0, "CIDB"), 0, true);
		users.info("CCCC", make("carol", 0, "CIDC"), 0, true);

		size_t seen = 0;
		for (QuickDC::User* u = users.first(); u; u = users.next())
		{
			seen++;
			/* Exactly what a search reply handler does mid-walk. */
			users.getUserByNick("carol");
			users.getUserByClientID("CIDA");
		}

		EXO_ASSERT(seen == 3);
	
	return 1;
});

/* Walking an empty manager, and next() without first(). */
EXO_TEST(users_walking_an_empty_manager_is_safe,
{
		QuickDC::UserManager users(0);
		EXO_ASSERT(users.first() == 0);
		EXO_ASSERT(users.next() == 0);

		QuickDC::UserManager fresh(0);
		EXO_ASSERT(fresh.next() == 0);
	
	return 1;
});

/* ---- operators ---- */
EXO_TEST(users_operators_are_counted_and_stop_being_counted_on_quit,
{
		QuickDC::UserManager users(0);
		users.info("AAAA", make("alice"), 0, true);
		users.info("BBBB", make("bob"), 0, true);

		EXO_ASSERT(users.countOperators() == 0);

		QuickDC::User* bob = (QuickDC::User*) users.getUser("BBBB");
		bob->setOp(true);

		EXO_ASSERT(bob->isOp());
		EXO_ASSERT(users.countOperators() == 1);

		QuickDC::User* alice = (QuickDC::User*) users.getUser("AAAA");
		alice->setOp(true);
		EXO_ASSERT(users.countOperators() == 2);

		users.quit("AAAA");
		EXO_ASSERT(users.countOperators() == 1);

	return 1;
});

/* $OpList is the whole answer, so it demotes as well as promotes. */
EXO_TEST(users_an_operator_list_promotes_and_demotes,
{
		QuickDC::UserManager users(0);
		users.info("alice", make("alice"), 0, true);
		users.info("bob", make("bob"), 0, true);

		users.setOperators({"alice"});
		EXO_ASSERT(users.getUser("alice")->isOp());
		EXO_ASSERT(!users.getUser("bob")->isOp());

		users.setOperators({"bob"});
		EXO_ASSERT(!users.getUser("alice")->isOp());
		EXO_ASSERT(users.getUser("bob")->isOp());

		users.setOperators({});
		EXO_ASSERT(users.countOperators() == 0);

	return 1;
});

/* The list can name somebody whose info has not been answered for yet. */
EXO_TEST(users_an_operator_named_before_arriving_is_still_marked,
{
		QuickDC::UserManager users(0);
		users.setOperators({"carol"});

		users.info("carol", make("carol"), 0, true);
		EXO_ASSERT(users.getUser("carol")->isOp());

	return 1;
});

/* A hub resends the list unchanged; nothing moved, so nobody is reported. */
EXO_TEST(users_an_unchanged_operator_list_reports_nothing,
{
		Events events;
		QuickDC::UserManager users(&events);
		users.info("alice", make("alice"), 0, true);
		users.info("bob", make("bob"), 0, true);

		users.setOperators({"alice"});
		const size_t reported = events.info.size();

		users.setOperators({"alice"});
		EXO_ASSERT(events.info.size() == reported);

	return 1;
});

/*
 * ADC says it in each user's own info and sends no list at all, so a manager
 * that has never been given one must leave what was set there alone.
 */
EXO_TEST(users_without_an_operator_list_nobody_is_demoted,
{
		QuickDC::UserManager users(0);
		QuickDC::User* alice = make("alice");
		alice->setOp(true);

		users.info("AAAA", alice, 0, true);
		EXO_ASSERT(users.getUser("AAAA")->isOp());

		users.info("AAAA", alice, 0, false);
		EXO_ASSERT(users.getUser("AAAA")->isOp());

	return 1;
});

/* ---- cleanup ---- */
EXO_TEST(users_cleanup_empties_the_hub,
{
		Events events;
		QuickDC::UserManager users(&events);
		users.info("AAAA", make("alice", 100), 0, true);
		users.info("BBBB", make("bob", 200), 0, true);

		users.cleanup();

		EXO_ASSERT(events.cleanups == 1);
		EXO_ASSERT(users.countUsers() == 0);
		EXO_ASSERT(users.getUser("AAAA") == 0);
		EXO_ASSERT(users.first() == 0);
	
	return 1;
});

/* The manager is reusable after a cleanup, as it is on a reconnect. */
EXO_TEST(users_the_hub_can_be_repopulated_after_a_cleanup,
{
		QuickDC::UserManager users(0);
		users.info("AAAA", make("alice", 100), 0, true);
		users.cleanup();

		users.info("AAAA", make("alice", 50), 0, true);
		EXO_ASSERT(users.countUsers() == 1);
		EXO_ASSERT(users.getUser("AAAA") != 0);
	
	return 1;
});

/* ------------------------------------------------------------------------- */
/* The features a user announced                                            */
/* ------------------------------------------------------------------------- */

/*
 * The SU field is a comma separated set, and what is asked of it decides whether a
 * peer gets told to do something. A search of the string would credit a peer with
 * whatever its flags happen to contain: "ADC0" contains "DC0", "NATT0" contains
 * "NATT", and a peer that cannot traverse would be sent to a port nothing answers.
 */
EXO_TEST(users_a_feature_is_matched_whole,
{
	QuickDC::User u("AAAA");
	u.setSupports("ADC0,TCP4,UDP4,NATT");

	EXO_ASSERT(u.supports("ADC0"));
	EXO_ASSERT(u.supports("TCP4"));
	EXO_ASSERT(u.supports("NATT"));

	/* Each of these is a substring of something above, and none was announced. */
	EXO_ASSERT(!u.supports("DC0"));
	EXO_ASSERT(!u.supports("CP4"));
	EXO_ASSERT(!u.supports("NAT"));
	EXO_ASSERT(!u.supports("ATT"));
	return 1;
});

/* Nor does a longer flag count as a shorter one it starts with. */
EXO_TEST(users_a_longer_flag_is_not_a_shorter_one,
{
	QuickDC::User u("AAAA");
	u.setSupports("ADC0,NATT0");

	EXO_ASSERT(!u.supports("NATT"));
	EXO_ASSERT(u.supports("NATT0"));
	return 1;
});

/* The first and last entries are entries too, which an implementation that only
   looked between separators would miss. */
EXO_TEST(users_the_first_and_last_features_count,
{
	QuickDC::User u("AAAA");
	u.setSupports("FIRST,MIDDLE,LAST");

	EXO_ASSERT(u.supports("FIRST"));
	EXO_ASSERT(u.supports("MIDDLE"));
	EXO_ASSERT(u.supports("LAST"));
	return 1;
});

/* And a user that announced nothing supports nothing. */
EXO_TEST(users_announcing_nothing_supports_nothing,
{
	QuickDC::User u("AAAA");

	EXO_ASSERT(!u.supports("NATT"));
	EXO_ASSERT(!u.supports(""));
	EXO_ASSERT(!u.supports(0));

	u.setSupports("");
	EXO_ASSERT(!u.supports("NATT"));
	return 1;
});
