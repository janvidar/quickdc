/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_NETWORKWATCH_H
#define HAVE_QUICKDC_NETWORKWATCH_H

#include <samurai/timer.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace Samurai { class EventLoop; }

namespace QuickDC {

/**
 * Notices this host moving to another network - its addresses changing, or a
 * front end that hears it from the platform saying so - and, once that has
 * settled and the host is online, says so once.
 */
class NetworkWatch final : public Samurai::TimerListener {
	public:
		struct Timing {
			std::chrono::milliseconds poll{15000};
			std::chrono::milliseconds settle{3000};
		};

		NetworkWatch(Samurai::EventLoop& loop, std::function<void()> settled,
		             std::function<std::string()> addresses, Timing timing);
		NetworkWatch(Samurai::EventLoop& loop, std::function<void()> settled);
		~NetworkWatch() override;

		NetworkWatch(const NetworkWatch&) = delete;
		NetworkWatch& operator=(const NetworkWatch&) = delete;

		/** The platform says the network changed; 'online' is whether anything is reachable. */
		void changed(bool online);

	private:
		void EventTimeout(Samurai::Timer* timer) override;
		void unsettle();

		Samurai::EventLoop& loop;
		std::function<void()> settled;
		std::function<std::string()> addresses;
		Timing timing;

		std::string seen;
		bool online = true;
		std::unique_ptr<Samurai::Timer> poll;
		std::unique_ptr<Samurai::Timer> settling;
};

}

#endif // HAVE_QUICKDC_NETWORKWATCH_H
