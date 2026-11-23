/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "api/inbox.h"

#include <samurai/io/net/notifier.h>

#include <utility>

QuickDC::Inbox::~Inbox()
{
	close();
}

bool QuickDC::Inbox::open()
{
	std::shared_ptr<Samurai::IO::Net::Notifier> made =
		Samurai::IO::Net::Notifier::create([] { });

	std::lock_guard<std::mutex> guard(mutex);
	notifier = std::move(made);
	accepting = true;
	return notifier != nullptr;
}

bool QuickDC::Inbox::post(Job job)
{
	if (!job) return false;

	std::shared_ptr<Samurai::IO::Net::Notifier> waking;
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (!accepting) return false;
		jobs.push_back(std::move(job));
		waking = notifier;
	}

	if (waking) waking->notify();
	return true;
}

void QuickDC::Inbox::wake()
{
	std::shared_ptr<Samurai::IO::Net::Notifier> waking;
	{
		std::lock_guard<std::mutex> guard(mutex);
		waking = notifier;
	}

	if (waking) waking->notify();
}

size_t QuickDC::Inbox::drain()
{
	std::deque<Job> batch;
	{
		std::lock_guard<std::mutex> guard(mutex);
		batch.swap(jobs);
	}

	for (Job& job : batch) job();
	return batch.size();
}

void QuickDC::Inbox::close()
{
	{
		std::lock_guard<std::mutex> guard(mutex);
		accepting = false;
	}

	/* A job run here may still post; the inbox is closed, so that is refused. */
	while (drain() > 0) { }

	std::shared_ptr<Samurai::IO::Net::Notifier> dropped;
	{
		std::lock_guard<std::mutex> guard(mutex);
		dropped.swap(notifier);
	}
}

bool QuickDC::Inbox::isOpen() const
{
	std::lock_guard<std::mutex> guard(mutex);
	return accepting;
}
