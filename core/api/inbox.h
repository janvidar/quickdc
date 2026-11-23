/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_INBOX_H
#define HAVE_QUICKDC_API_INBOX_H

#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace Samurai { namespace IO { namespace Net { class Notifier; } } }

namespace QuickDC {

/**
 * Work handed to the thread that drives a core, from any thread.
 *
 * open(), drain() and close() belong to that thread; post() and wake() may be
 * called from anywhere. Jobs run in the order they were posted, on the owning
 * thread, inside drain() or close().
 */
class Inbox {
	public:
		using Job = std::function<void()>;

		Inbox() = default;
		~Inbox();

		Inbox(const Inbox&) = delete;
		Inbox& operator=(const Inbox&) = delete;

		/**
		 * Start accepting jobs, and make post() interrupt the socket monitor's
		 * wait so that one is not left waiting for the next poll interval.
		 *
		 * The wake-up does nothing but wake. Jobs run in drain(), between
		 * turns, never from inside the monitor's dispatch - where one that
		 * deleted a socket would be changing what the monitor is walking.
		 *
		 * @return false where no wake-up could be made; jobs are then still
		 *         accepted and run, only later.
		 */
		bool open();

		/**
		 * Queue 'job' and wake the owning thread.
		 *
		 * @return false where the inbox is not open, in which case the job is
		 *         destroyed unrun - on the calling thread.
		 */
		bool post(Job job);

		/** Interrupt the owning thread's wait without queueing anything. */
		void wake();

		/** Run every job queued so far; ones they post wait for the next call. */
		size_t drain();

		/** Stop accepting jobs, run the ones already queued, and drop the wake-up. */
		void close();

		bool isOpen() const;

	private:
		mutable std::mutex mutex;
		std::deque<Job> jobs;
		std::shared_ptr<Samurai::IO::Net::Notifier> notifier;
		bool accepting = false;
};

}

#endif // HAVE_QUICKDC_API_INBOX_H
