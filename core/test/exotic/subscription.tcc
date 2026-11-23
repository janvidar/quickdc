/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Subscriptions and the listener lists they come from.
 *
 * The cases that matter are the ones a vector of raw pointers gets wrong: a
 * listener leaving in the middle of a dispatch, a list destroyed by one of its
 * own listeners, and a subscription outliving the list it was made on.
 */

#include "quickdc.h"
#include "api/subscription.h"

#include <memory>
#include <vector>

namespace {

class Counter {
	public:
		virtual ~Counter() { }
		virtual void tick() { count++; }
		int count = 0;
};

using List = QuickDC::ListenerList<Counter>;

class Leaver : public Counter {
	public:
		QuickDC::Subscription* own = nullptr;
		void tick() override { count++; if (own) own->reset(); }
};

class Remover : public Counter {
	public:
		List* list = nullptr;
		Counter* victim = nullptr;
		void tick() override { count++; list->remove(victim); }
};

class Destroyer : public Counter {
	public:
		std::unique_ptr<List>* list = nullptr;
		void tick() override { count++; list->reset(); }
};

class Joiner : public Counter {
	public:
		List* list = nullptr;
		Counter* newcomer = nullptr;
		std::vector<QuickDC::Subscription>* keep = nullptr;
		void tick() override
		{
			count++;
			if (newcomer) { keep->push_back(list->subscribe(newcomer)); newcomer = nullptr; }
		}
};

class Reentrant : public Counter {
	public:
		List* list = nullptr;
		void tick() override
		{
			if (count++ == 0) list->notify([](Counter& c) { c.tick(); });
		}
};

void tickAll(List& list)
{
	list.notify([](Counter& c) { c.tick(); });
}

}

EXO_TEST(subscription_a_subscriber_is_called,
{
	List list;
	Counter a;
	QuickDC::Subscription s = list.subscribe(&a);

	tickAll(list);
	EXO_ASSERT_EQ_INT(a.count, 1);
	EXO_ASSERT(s.active());
	EXO_ASSERT_EQ_UINT(list.size(), 1u);
	return 1;
});

EXO_TEST(subscription_destroying_it_unsubscribes,
{
	List list;
	Counter a;
	{
		QuickDC::Subscription s = list.subscribe(&a);
	}

	tickAll(list);
	EXO_ASSERT_EQ_INT(a.count, 0);
	EXO_ASSERT(list.empty());
	return 1;
});

EXO_TEST(subscription_reset_unsubscribes_and_is_idempotent,
{
	List list;
	Counter a;
	QuickDC::Subscription s = list.subscribe(&a);

	s.reset();
	s.reset();
	tickAll(list);
	EXO_ASSERT_EQ_INT(a.count, 0);
	EXO_ASSERT(!s.active());
	return 1;
});

EXO_TEST(subscription_moving_it_keeps_one_subscription,
{
	List list;
	Counter a;
	QuickDC::Subscription s = list.subscribe(&a);
	QuickDC::Subscription moved = std::move(s);

	EXO_ASSERT(!s.active());
	EXO_ASSERT(moved.active());
	tickAll(list);
	EXO_ASSERT_EQ_INT(a.count, 1);

	moved = QuickDC::Subscription();
	tickAll(list);
	EXO_ASSERT_EQ_INT(a.count, 1);
	return 1;
});

EXO_TEST(subscription_outliving_its_list_is_harmless,
{
	Counter a;
	QuickDC::Subscription s;
	{
		List list;
		s = list.subscribe(&a);
	}

	EXO_ASSERT(!s.active());
	s.reset();
	return 1;
});

EXO_TEST(subscription_leaving_during_dispatch_skips_nobody_else,
{
	List list;
	Counter before, after;
	Leaver leaver;

	QuickDC::Subscription s1 = list.subscribe(&before);
	QuickDC::Subscription s2 = list.subscribe(&leaver);
	QuickDC::Subscription s3 = list.subscribe(&after);
	leaver.own = &s2;

	tickAll(list);
	EXO_ASSERT_EQ_INT(before.count, 1);
	EXO_ASSERT_EQ_INT(leaver.count, 1);
	EXO_ASSERT_EQ_INT(after.count, 1);

	tickAll(list);
	EXO_ASSERT_EQ_INT(leaver.count, 1);
	EXO_ASSERT_EQ_INT(after.count, 2);
	EXO_ASSERT_EQ_UINT(list.size(), 2u);
	return 1;
});

EXO_TEST(subscription_one_removed_by_another_during_dispatch_is_not_called,
{
	List list;
	Remover remover;
	Counter victim;

	remover.list = &list;
	remover.victim = &victim;
	list.add(&remover);
	list.add(&victim);

	tickAll(list);
	EXO_ASSERT_EQ_INT(remover.count, 1);
	EXO_ASSERT_EQ_INT(victim.count, 0);
	EXO_ASSERT(!list.contains(&victim));
	return 1;
});

EXO_TEST(subscription_one_added_during_dispatch_waits_for_the_next,
{
	List list;
	Joiner joiner;
	Counter newcomer;
	std::vector<QuickDC::Subscription> keep;

	joiner.list = &list;
	joiner.newcomer = &newcomer;
	joiner.keep = &keep;
	QuickDC::Subscription s = list.subscribe(&joiner);

	tickAll(list);
	EXO_ASSERT_EQ_INT(newcomer.count, 0);

	tickAll(list);
	EXO_ASSERT_EQ_INT(newcomer.count, 1);
	return 1;
});

EXO_TEST(subscription_a_list_destroyed_during_dispatch_ends_it,
{
	auto list = std::make_unique<List>();
	Destroyer destroyer;
	Counter after;

	destroyer.list = &list;
	QuickDC::Subscription s1 = list->subscribe(&destroyer);
	QuickDC::Subscription s2 = list->subscribe(&after);

	tickAll(*list);
	EXO_ASSERT(!list);
	EXO_ASSERT_EQ_INT(destroyer.count, 1);
	EXO_ASSERT_EQ_INT(after.count, 0);
	EXO_ASSERT(!s1.active());
	EXO_ASSERT(!s2.active());
	return 1;
});

EXO_TEST(subscription_a_nested_dispatch_keeps_entries_until_the_outer_one_ends,
{
	List list;
	Reentrant reentrant;
	Leaver leaver;

	reentrant.list = &list;
	QuickDC::Subscription s1 = list.subscribe(&reentrant);
	QuickDC::Subscription s2 = list.subscribe(&leaver);
	leaver.own = &s2;

	tickAll(list);
	EXO_ASSERT_EQ_INT(reentrant.count, 2);
	EXO_ASSERT_EQ_INT(leaver.count, 1);
	EXO_ASSERT_EQ_UINT(list.size(), 1u);
	return 1;
});

EXO_TEST(subscription_add_by_pointer_does_not_duplicate,
{
	List list;
	Counter a;

	list.add(&a);
	list.add(&a);
	list.add(nullptr);
	tickAll(list);
	EXO_ASSERT_EQ_INT(a.count, 1);

	list.remove(&a);
	tickAll(list);
	EXO_ASSERT_EQ_INT(a.count, 1);
	return 1;
});

EXO_TEST(subscription_subscribing_nothing_is_inactive,
{
	List list;
	QuickDC::Subscription s = list.subscribe(nullptr);
	EXO_ASSERT(!s.active());
	EXO_ASSERT(list.empty());
	return 1;
});
