/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "runner.h"

#include "api/core.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

class Mailbox {
	public:
		void push(QuickDC::Cli::Delivery delivery)
		{
			{
				std::lock_guard<std::mutex> guard(mutex);
				queue.push_back(std::move(delivery));
			}
			arrived.notify_one();
		}

		std::optional<QuickDC::Cli::Delivery> pop(std::chrono::steady_clock::time_point deadline)
		{
			std::unique_lock<std::mutex> lock(mutex);
			if (!arrived.wait_until(lock, deadline, [this] { return !queue.empty(); }))
				return std::nullopt;

			QuickDC::Cli::Delivery delivery = std::move(queue.front());
			queue.pop_front();
			return delivery;
		}

	private:
		std::mutex mutex;
		std::condition_variable arrived;
		std::deque<QuickDC::Cli::Delivery> queue;
};

class Forward final : public QuickDC::Api::EventSink {
	public:
		Forward(Mailbox& mailbox, std::string core) : mailbox(mailbox), core(std::move(core)) { }

		void deliver(const QuickDC::Api::Event& event) override
		{
			mailbox.push(QuickDC::Cli::Delivery{core, event});
		}

	private:
		Mailbox& mailbox;
		std::string core;
};

enum class State { Starting, Running, Failed, Stopped };

struct Host {
	std::string name;
	std::string home;
	QuickDC::Api::SettingsSnapshot presets;

	std::mutex mutex;
	std::condition_variable changed;
	State state = State::Starting;
	bool stopRequested = false;
	QuickDC::Core* core = nullptr;
	std::string failure;

	std::thread thread;

	void enter(State next)
	{
		{
			std::lock_guard<std::mutex> guard(mutex);
			state = next;
		}
		changed.notify_all();
	}

	void run(Mailbox& mailbox)
	{
		std::unique_ptr<QuickDC::Core> built;

		try
		{
			/* A script opens the hubs it means to, its own included; a preset can say otherwise. */
			auto application = presets.application;
			application["Local hub"].emplace("Join", "0");
			built = std::make_unique<QuickDC::Core>(
				QuickDC::Core::Home{home, application, presets.session});
		}
		catch (const std::exception& e)
		{
			std::lock_guard<std::mutex> guard(mutex);
			failure = e.what();
		}
		catch (...)
		{
			std::lock_guard<std::mutex> guard(mutex);
			failure = "the core threw an unrecognised exception";
		}

		if (!built)
		{
			enter(State::Failed);
			return;
		}

		{
			std::lock_guard<std::mutex> guard(mutex);
			core = built.get();
			if (stopRequested) core->requestStop();
		}

		Forward forward(mailbox, name);
		QuickDC::Subscription events = built->subscribe(&forward);

		built->runUntilStopped([this] { enter(State::Running); });

		{
			std::lock_guard<std::mutex> guard(mutex);
			core = nullptr;
		}

		events.reset();
		built.reset();

		enter(State::Stopped);
	}
};

}

struct QuickDC::Cli::Runner::Impl {
	Mailbox mailbox;
	std::vector<std::unique_ptr<Host>> hosts;

	Host* find(const std::string& name)
	{
		for (const std::unique_ptr<Host>& host : hosts)
			if (host->name == name) return host.get();
		return nullptr;
	}
};

QuickDC::Cli::Runner::Runner() : impl(std::make_unique<Impl>()) { }

QuickDC::Cli::Runner::~Runner()
{
	stop();
}

bool QuickDC::Cli::Runner::start(const std::string& name, const std::string& home,
                        const QuickDC::Api::SettingsSnapshot& presets, std::string& failure)
{
	Host* existing = impl->find(name);
	if (existing)
	{
		std::lock_guard<std::mutex> guard(existing->mutex);
		if (existing->state != State::Stopped && existing->state != State::Failed)
		{
			failure = "there is already a core called '" + name + "'";
			return false;
		}
	}

	if (!existing)
	{
		impl->hosts.push_back(std::make_unique<Host>());
		existing = impl->hosts.back().get();
	}

	Host& host = *existing;
	if (host.thread.joinable()) host.thread.join();
	host.state = State::Starting;
	host.stopRequested = false;
	host.failure.clear();
	host.name = name;
	host.home = home;
	host.presets = presets;
	host.thread = std::thread([&host, &mailbox = impl->mailbox] { host.run(mailbox); });

	std::unique_lock<std::mutex> lock(host.mutex);
	host.changed.wait(lock, [&host] { return host.state != State::Starting; });

	if (host.state == State::Running) return true;

	failure = host.failure.empty() ? "the core could not be built" : host.failure;
	return false;
}

bool QuickDC::Cli::Runner::submit(const std::string& name, QuickDC::Api::Command command)
{
	Host* host = impl->find(name);
	if (!host) return false;

	std::lock_guard<std::mutex> guard(host->mutex);
	if (host->state != State::Running || !host->core) return false;
	return host->core->submit(std::move(command));
}

std::optional<QuickDC::Cli::Delivery> QuickDC::Cli::Runner::next(std::chrono::steady_clock::time_point deadline)
{
	return impl->mailbox.pop(deadline);
}

void QuickDC::Cli::Runner::stop()
{
	/* Every core asked first, so they wind down together rather than one after another. */
	for (const std::unique_ptr<Host>& host : impl->hosts)
	{
		std::lock_guard<std::mutex> guard(host->mutex);
		host->stopRequested = true;
		if (host->core) host->core->requestStop();
	}

	for (const std::unique_ptr<Host>& host : impl->hosts)
		if (host->thread.joinable()) host->thread.join();
}

void QuickDC::Cli::Runner::stop(const std::string& name)
{
	Host* host = impl->find(name);
	if (!host) return;

	{
		std::lock_guard<std::mutex> guard(host->mutex);
		host->stopRequested = true;
		if (host->core) host->core->requestStop();
	}

	if (host->thread.joinable()) host->thread.join();
}
