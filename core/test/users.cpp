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

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

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

int main()
{
	setbuf(stdout, 0);

	/* ---- joining and leaving ---- */

	{
		Events events;
		QuickDC::UserManager users(&events);

		users.join("AAAA");
		CHECK(users.countUsers() == 1);
		CHECK(users.getUser("AAAA") != 0);
		CHECK(events.joined.size() == 1);

		users.quit("AAAA");
		CHECK(users.countUsers() == 0);
		CHECK(users.getUser("AAAA") == 0);
		CHECK(events.quit.size() == 1);
		printf("  ok a user joins and leaves\n");
	}

	/* The same id twice is one user; a hub may repeat itself. */
	{
		QuickDC::UserManager users(0);
		users.join("AAAA");
		users.join("AAAA");
		CHECK(users.countUsers() == 1);
		printf("  ok joining twice is still one user\n");
	}

	/* Leaving without having joined must not invent anyone. */
	{
		Events events;
		QuickDC::UserManager users(&events);
		users.quit("NOBODY");
		CHECK(users.countUsers() == 0);
		CHECK(events.quit.empty());
		printf("  ok quitting without joining does nothing\n");
	}

	/*
	 * A null id is what a malformed command leaves behind, and every one of
	 * these builds a std::string from it.
	 */
	{
		QuickDC::UserManager users(0);
		users.join(0);
		users.quit(0);
		users.op(0);
		users.ip(0, "127.0.0.1");
		users.info(0, make("x"), 0, true);

		CHECK(users.countUsers() == 0);
		CHECK(users.getUser(0) == 0);
		CHECK(users.getUserByNick(0) == 0);
		CHECK(users.getUserByClientID(0) == 0);
		CHECK(users.getUserByClientID("") == 0);
		printf("  ok a null id is refused everywhere\n");
	}

	/* ---- info ---- */

	{
		Events events;
		QuickDC::UserManager users(&events);

		users.info("AAAA", make("alice", 100), 0, true);
		CHECK(users.countUsers() == 1);
		CHECK(events.joined.size() == 1);
		CHECK(users.countShared() == 100);

		const QuickDC::User* u = users.getUser("AAAA");
		CHECK(u && u->getNick() && !strcmp(u->getNick(), "alice"));
		printf("  ok info about an unknown user adds them\n");
	}

	/*
	 * An update hands back the same object, mutated, with what it used to
	 * share - which is how the running total stays right.
	 */
	{
		QuickDC::UserManager users(0);
		users.info("AAAA", make("alice", 100), 0, true);

		QuickDC::User* u = (QuickDC::User*) users.getUser("AAAA");
		const uint64_t was = u->getSharedBytes();
		u->setSharedBytes(250);
		users.info("AAAA", u, was, true);

		CHECK(users.countUsers() == 1);
		CHECK(users.countShared() == 250);
		printf("  ok an update replaces the old share rather than adding to it\n");
	}

	{
		QuickDC::UserManager users(0);
		users.info("AAAA", make("alice", 100), 0, true);
		users.info("BBBB", make("bob", 400), 0, true);

		CHECK(users.countUsers() == 2);
		CHECK(users.countShared() == 500);

		users.quit("AAAA");
		CHECK(users.countShared() == 400);
		printf("  ok the shared total follows who is on the hub\n");
	}

	/* An unchanged update is not reported to the interface. */
	{
		Events events;
		QuickDC::UserManager users(&events);
		users.info("AAAA", make("alice"), 0, true);

		QuickDC::User* u = (QuickDC::User*) users.getUser("AAAA");
		events.info.clear();

		users.info("AAAA", u, u->getSharedBytes(), false);
		CHECK(events.info.empty());

		users.info("AAAA", u, u->getSharedBytes(), true);
		CHECK(events.info.size() == 1);
		printf("  ok only a changed update is reported\n");
	}

	/* ---- lookups ---- */

	{
		QuickDC::UserManager users(0);
		users.info("AAAA", make("alice", 0, "CIDALICE"), 0, true);
		users.info("BBBB", make("bob", 0, "CIDBOB"), 0, true);

		CHECK(users.getUser("AAAA") != 0);
		CHECK(users.getUser("ZZZZ") == 0);

		const QuickDC::User* byNick = users.getUserByNick("bob");
		CHECK(byNick && byNick->getNick() && !strcmp(byNick->getNick(), "bob"));
		CHECK(users.getUserByNick("nobody") == 0);

		const QuickDC::User* byCid = users.getUserByClientID("CIDALICE");
		CHECK(byCid && byCid->getNick() && !strcmp(byCid->getNick(), "alice"));
		CHECK(users.getUserByClientID("CIDNOBODY") == 0);
		printf("  ok users are found by id, nick and client id\n");
	}

	/*
	 * Looking a user up from inside a walk must not move the walk. The two
	 * lookups above used to share the manager's single iterator with it.
	 */
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

		CHECK(seen == 3);
		printf("  ok a lookup during a walk does not disturb the walk\n");
	}

	/* Walking an empty manager, and next() without first(). */
	{
		QuickDC::UserManager users(0);
		CHECK(users.first() == 0);
		CHECK(users.next() == 0);

		QuickDC::UserManager fresh(0);
		CHECK(fresh.next() == 0);
		printf("  ok walking an empty manager is safe\n");
	}

	/* ---- operators ---- */

	{
		QuickDC::UserManager users(0);
		users.info("AAAA", make("alice"), 0, true);
		users.info("BBBB", make("bob"), 0, true);

		CHECK(users.countOperators() == 0);

		QuickDC::User* bob = (QuickDC::User*) users.getUser("BBBB");
		bob->setOp();

		CHECK(bob->isOp());
		CHECK(users.countOperators() == 1);

		QuickDC::User* alice = (QuickDC::User*) users.getUser("AAAA");
		alice->setOp();
		CHECK(users.countOperators() == 2);

		users.quit("AAAA");
		CHECK(users.countOperators() == 1);
		printf("  ok operators are counted, and stop being counted on quit\n");
	}

	/* ---- cleanup ---- */

	{
		Events events;
		QuickDC::UserManager users(&events);
		users.info("AAAA", make("alice", 100), 0, true);
		users.info("BBBB", make("bob", 200), 0, true);

		users.cleanup();

		CHECK(events.cleanups == 1);
		CHECK(users.countUsers() == 0);
		CHECK(users.getUser("AAAA") == 0);
		CHECK(users.first() == 0);
		printf("  ok cleanup empties the hub\n");
	}

	/* The manager is reusable after a cleanup, as it is on a reconnect. */
	{
		QuickDC::UserManager users(0);
		users.info("AAAA", make("alice", 100), 0, true);
		users.cleanup();

		users.info("AAAA", make("alice", 50), 0, true);
		CHECK(users.countUsers() == 1);
		CHECK(users.getUser("AAAA") != 0);
		printf("  ok the hub can be repopulated after a cleanup\n");
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
