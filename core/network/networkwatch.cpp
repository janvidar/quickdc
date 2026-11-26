/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/networkwatch.h"
#include "network/localaddresses.h"

#include <samurai/eventloop.h>

#include <utility>

QuickDC::NetworkWatch::NetworkWatch(Samurai::EventLoop& loop_, std::function<void()> settled_,
                                    std::function<std::string()> addresses_, Timing timing_)
	: loop(loop_)
	, settled(std::move(settled_))
	, addresses(std::move(addresses_))
	, timing(timing_)
	, seen(addresses())
	, poll(std::make_unique<Samurai::Timer>(loop_, this, timing_.poll, false))
{
}

QuickDC::NetworkWatch::NetworkWatch(Samurai::EventLoop& loop_, std::function<void()> settled_)
	: NetworkWatch(loop_, std::move(settled_), addressFingerprint, Timing())
{
}

QuickDC::NetworkWatch::~NetworkWatch() = default;

void QuickDC::NetworkWatch::changed(bool online_)
{
	online = online_;
	unsettle();
}

/* Restarted by each change, so a burst of them is one report after it. */
void QuickDC::NetworkWatch::unsettle()
{
	settling = std::make_unique<Samurai::Timer>(loop, this, timing.settle, true);
}

void QuickDC::NetworkWatch::EventTimeout(Samurai::Timer* timer)
{
	if (timer == poll.get())
	{
		std::string now = addresses();
		if (now == seen) return;
		seen = std::move(now);
		unsettle();
		return;
	}

	/* The settle timer is single shot; it stays until the next change replaces it. */
	if (online && settled) settled();
}
