/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_SUBSCRIPTION_H
#define HAVE_QUICKDC_API_SUBSCRIPTION_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace QuickDC {

namespace Detail {

/** What a Subscription calls back into, without knowing the listener type. */
class SubscriptionTarget {
	public:
		virtual ~SubscriptionTarget() { }
		virtual void unsubscribe(uint64_t id) = 0;
};

}

/**
 * Being subscribed, as an object: destroying it, or calling reset(), ends it.
 *
 * Safe whatever has happened to the list it came from. A list destroyed first
 * leaves the subscription with nothing to undo, and one ended from inside the
 * dispatch it belongs to is not called again, in that dispatch or any later one.
 *
 * Not thread-safe: made, moved and destroyed on the thread that owns the list,
 * which for the core's lists is the thread that drives the core.
 */
class [[nodiscard]] Subscription {
	public:
		Subscription() = default;

		Subscription(std::weak_ptr<Detail::SubscriptionTarget> target, uint64_t id)
			: target(std::move(target))
			, id(id)
		{
		}

		Subscription(Subscription&& other) noexcept
			: target(std::move(other.target))
			, id(other.id)
		{
			other.target.reset();
		}

		Subscription& operator=(Subscription&& other) noexcept
		{
			if (this != &other)
			{
				reset();
				target = std::move(other.target);
				id = other.id;
				other.target.reset();
			}
			return *this;
		}

		Subscription(const Subscription&) = delete;
		Subscription& operator=(const Subscription&) = delete;

		~Subscription() { reset(); }

		void reset()
		{
			if (auto live = target.lock()) live->unsubscribe(id);
			target.reset();
		}

		/** Whether this still names a subscription on a list that exists. */
		bool active() const { return !target.expired(); }

	private:
		std::weak_ptr<Detail::SubscriptionTarget> target;
		uint64_t id = 0;
};

/**
 * The listeners of one kind of event, dispatched to safely.
 *
 * During notify() a listener may unsubscribe itself or any other, subscribe a
 * new one, or destroy the list: the one removed is skipped, the one added is
 * first called by the next dispatch, and a destroyed list ends the dispatch
 * without touching freed memory.
 *
 * The pointer-taking add() and remove() are for registries still being moved
 * over to subscribe(); a listener added that way is removed by pointer.
 */
template<typename Listener>
class ListenerList {
	public:
		ListenerList() : state(std::make_shared<State>()) { }

		~ListenerList() { state->alive = false; }

		ListenerList(const ListenerList&) = delete;
		ListenerList& operator=(const ListenerList&) = delete;

		Subscription subscribe(Listener* listener)
		{
			if (!listener) return Subscription();
			return Subscription(state, state->insert(listener));
		}

		void add(Listener* listener)
		{
			if (!listener || contains(listener)) return;
			state->insert(listener);
		}

		void remove(Listener* listener)
		{
			for (auto& entry : state->entries)
				if (entry.listener == listener) entry.listener = nullptr;
			state->settle();
		}

		bool contains(const Listener* listener) const
		{
			for (const auto& entry : state->entries)
				if (entry.listener == listener) return true;
			return false;
		}

		bool empty() const { return size() == 0; }

		size_t size() const
		{
			size_t count = 0;
			for (const auto& entry : state->entries)
				if (entry.listener) count++;
			return count;
		}

		/** Call 'call(listener)' for every listener subscribed when this began. */
		template<typename Call>
		void notify(Call&& call)
		{
			std::shared_ptr<State> held = state;
			const size_t end = held->entries.size();

			held->depth++;
			for (size_t n = 0; n < end && held->alive; n++)
			{
				Listener* listener = held->entries[n].listener;
				if (listener) call(*listener);
			}
			held->depth--;

			if (held->depth == 0) held->compact();
		}

	private:
		struct Entry {
			uint64_t id;
			Listener* listener;
		};

		struct State : public Detail::SubscriptionTarget {
			std::vector<Entry> entries;
			uint64_t next = 1;
			int depth = 0;
			bool alive = true;

			uint64_t insert(Listener* listener)
			{
				const uint64_t id = next++;
				entries.push_back(Entry{id, listener});
				return id;
			}

			/* Removed entries are nulled first, so a dispatch in progress keeps its indices. */
			void settle()
			{
				if (depth == 0) compact();
			}

			void compact()
			{
				std::erase_if(entries, [](const Entry& e) { return !e.listener; });
			}

			void unsubscribe(uint64_t id) override
			{
				for (auto& entry : entries)
					if (entry.id == id) entry.listener = nullptr;
				settle();
			}
		};

		std::shared_ptr<State> state;
};

}

#endif // HAVE_QUICKDC_API_SUBSCRIPTION_H
