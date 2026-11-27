/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The command and event boundary a front end drives the core through.
 *
 * Each case runs the core the way a front end does - its loop on a thread of its
 * own, commands submitted from this one, events taken off a sink - because that
 * crossing is what the boundary is for.
 */

#include "quickdc.h"
#include "api/core.h"
#include "api/protocol.h"
#include "config/preferences.h"

#include <samurai/io/file.h>

#include <stdlib.h>
#include <samurai/posix.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_service_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

/* Every event delivered, copied off the core's thread. */
class Recorder : public QuickDC::Api::EventSink {
	public:
		void deliver(const QuickDC::Api::Event& event) override
		{
			std::lock_guard<std::mutex> guard(mutex);
			events.push_back(event);
			arrived.notify_all();
		}

		bool waitFor(size_t count, std::chrono::seconds limit = std::chrono::seconds(10))
		{
			std::unique_lock<std::mutex> lock(mutex);
			return arrived.wait_for(lock, limit, [&] { return events.size() >= count; });
		}

		/* The first event of type T that meets 'wanted', waiting for it if need be. */
		template<typename T, typename Wanted>
		std::optional<T> waitForEvent(Wanted wanted, std::chrono::seconds limit = std::chrono::seconds(10))
		{
			std::unique_lock<std::mutex> lock(mutex);
			std::optional<T> found;
			arrived.wait_for(lock, limit, [&] {
				for (const QuickDC::Api::Event& event : events)
					if (const T* e = event.as<T>(); e && wanted(*e))
					{
						found = *e;
						return true;
					}
				return false;
			});
			return found;
		}

		/* How many events of type T arrived; others (a status, a network note) may come between. */
		template<typename T>
		size_t count()
		{
			std::lock_guard<std::mutex> guard(mutex);
			return countLocked<T>();
		}

		template<typename T>
		bool waitForCount(size_t n, std::chrono::seconds limit = std::chrono::seconds(10))
		{
			std::unique_lock<std::mutex> lock(mutex);
			return arrived.wait_for(lock, limit, [&] { return countLocked<T>() >= n; });
		}

		std::vector<QuickDC::Api::Event> taken()
		{
			std::lock_guard<std::mutex> guard(mutex);
			return events;
		}

	private:
		template<typename T>
		size_t countLocked() const
		{
			size_t n = 0;
			for (const QuickDC::Api::Event& event : events)
				if (event.as<T>()) n++;
			return n;
		}

		std::mutex mutex;
		std::condition_variable arrived;
		std::vector<QuickDC::Api::Event> events;
};

/* A scratch core whose loop runs on a thread of its own for as long as this lives. */
class Running {
	public:
		Running()
			: core(new QuickDC::Core(cfg, ses))
		{
			std::mutex mutex;
			std::condition_variable started;
			bool up = false;

			loop = std::thread([&] {
				core->runUntilStopped([&] {
					std::lock_guard<std::mutex> guard(mutex);
					up = true;
					started.notify_all();
				});
			});

			std::unique_lock<std::mutex> lock(mutex);
			started.wait(lock, [&] { return up; });
		}

		~Running()
		{
			core->requestStop();
			loop.join();
			delete core;
			Samurai::IO::File(cfg).remove();
			Samurai::IO::File(ses).remove();
		}

		/* Run 'job' on the core's thread and wait for it. */
		template<typename Job>
		void onCore(Job job)
		{
			std::mutex mutex;
			std::condition_variable done;
			bool finished = false;

			core->post([&] {
				job();
				std::lock_guard<std::mutex> guard(mutex);
				finished = true;
				done.notify_all();
			});

			std::unique_lock<std::mutex> lock(mutex);
			done.wait(lock, [&] { return finished; });
		}

		std::string cfg = scratch("config.conf");
		std::string ses = scratch("session.conf");
		QuickDC::Core* core;

	private:
		std::thread loop;
};

}

EXO_TEST(service_a_command_is_refused_while_the_core_is_not_running,
{
	const std::string cfg = scratch("config.conf");
	const std::string ses = scratch("session.conf");
	{
		QuickDC::Core core(cfg, ses);
		EXO_ASSERT(!core.submit(QuickDC::Api::Commands::FetchHubList{"http://127.0.0.1:1/"}));
	}
	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();
	return 1;
});

EXO_TEST(service_a_hublist_nobody_serves_is_reported_as_failed,
{
	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	const std::string url = "http://127.0.0.1:1/hublist.xml";
	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::FetchHubList{url}));
	const auto failed = recorder.waitForEvent<QuickDC::Api::Events::HubListFailed>(
		[](const QuickDC::Api::Events::HubListFailed&) { return true; });
	EXO_ASSERT(failed.has_value());
	if (failed)
	{
		EXO_ASSERT(failed->url == url);
		EXO_ASSERT(!failed->reason.empty());
	}

	running.onCore([&] { subscription.reset(); });
	return 1;
});

EXO_TEST(service_an_address_that_is_not_one_is_reported_as_failed,
{
	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::FetchHubList{"not an address"}));
	EXO_ASSERT(recorder.waitForCount<QuickDC::Api::Events::HubListFailed>(1));

	running.onCore([&] { subscription.reset(); });
	return 1;
});

/* A sink whose subscription ended hears nothing more, and every other one still does. */
EXO_TEST(service_an_ended_subscription_is_told_nothing_more,
{
	Running running;
	Recorder gone;
	Recorder staying;
	QuickDC::Subscription a;
	QuickDC::Subscription b;
	running.onCore([&] {
		a = running.core->subscribe(&gone);
		b = running.core->subscribe(&staying);
	});

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::FetchHubList{"not an address"}));
	using Failed = QuickDC::Api::Events::HubListFailed;
	EXO_ASSERT(staying.waitForCount<Failed>(1) && gone.waitForCount<Failed>(1));

	running.onCore([&] { a.reset(); });

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::FetchHubList{"still not one"}));
	EXO_ASSERT(staying.waitForCount<Failed>(2));
	EXO_ASSERT_EQ_UINT(gone.count<Failed>(), 1u);

	running.onCore([&] { b.reset(); });
	return 1;
});

/* A source that cannot name a file list is refused rather than left waiting. */
EXO_TEST(service_a_file_list_nobody_could_be_asked_for_is_refused,
{
	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::RequestFilelist{
		"adc://hub.example.org:412", "../escape"}));
	const auto failed = recorder.waitForEvent<QuickDC::Api::Events::FilelistFailed>(
		[](const QuickDC::Api::Events::FilelistFailed&) { return true; });
	EXO_ASSERT(failed.has_value());
	if (failed) EXO_ASSERT(failed->sourceId == "../escape");

	running.onCore([&] { subscription.reset(); });
	return 1;
});

/* The core turns a mode into the settings it decides, and says what is in force. */
EXO_TEST(service_choosing_the_connection_mode_writes_what_it_decides,
{
	using QuickDC::NetworkMode;
	namespace Ev = QuickDC::Api::Events;

	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::SetSetting{
		QuickDC::Api::SettingsStore::Application, "Network", "Active mode", "true", false, 1}));
	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::SetConnectionMode{
		NetworkMode::Passive, true, true, 2}));

	const std::optional<Ev::ConnectionMode> passive = recorder.waitForEvent<Ev::ConnectionMode>(
		[](const Ev::ConnectionMode& e) { return e.mode == NetworkMode::Passive; });
	EXO_ASSERT(passive.has_value());
	if (passive)
	{
		EXO_ASSERT(!passive->activeMode);
		EXO_ASSERT(!passive->useUpnp);
		EXO_ASSERT(!passive->forceActive);
		EXO_ASSERT(!passive->reachabilityServer.empty());
	}

	bool stored = true;
	running.onCore([&] { stored = running.core->config->peekBool("Network", "Active mode", true); });
	EXO_ASSERT(!stored);

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::SetConnectionMode{
		NetworkMode::Advanced, false, true, 3}));
	const std::optional<Ev::ConnectionMode> advanced = recorder.waitForEvent<Ev::ConnectionMode>(
		[](const Ev::ConnectionMode& e) { return e.mode == NetworkMode::Advanced && e.forceActive; });
	EXO_ASSERT(advanced.has_value());
	if (advanced)
	{
		EXO_ASSERT(advanced->activeMode);
		EXO_ASSERT(!advanced->useUpnp);
	}

	running.onCore([&] { subscription.reset(); });
	return 1;
});

/* A mode written as a setting is announced as one chosen by command is. */
EXO_TEST(service_a_mode_written_as_a_setting_is_announced,
{
	using QuickDC::NetworkMode;
	namespace Ev = QuickDC::Api::Events;

	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::SetSetting{
		QuickDC::Api::SettingsStore::Application, "Network", "Mode", "passive", false, 1}));
	EXO_ASSERT(recorder.waitForEvent<Ev::ConnectionMode>(
		[](const Ev::ConnectionMode& e) { return e.mode == NetworkMode::Passive && !e.activeMode; }));

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::ReportState{7}));
	EXO_ASSERT(recorder.waitForEvent<Ev::ConnectionMode>(
		[](const Ev::ConnectionMode& e) { return e.request == 7 && e.mode == NetworkMode::Passive; }));

	running.onCore([&] { subscription.reset(); });
	return 1;
});

/* Opening a hub is a visit, recorded by the core whatever opened it. */
EXO_TEST(service_opening_a_hub_records_a_visit,
{
	namespace Ev = QuickDC::Api::Events;

	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::OpenHub{1, "adc://127.0.0.1:1"}));
	const std::optional<Ev::RecentHubs> recent = recorder.waitForEvent<Ev::RecentHubs>(
		[](const Ev::RecentHubs& e) { return !e.hubs.empty(); });
	EXO_ASSERT(recent.has_value());
	if (recent)
	{
		EXO_ASSERT(recent->hubs[0].url == "adc://127.0.0.1:1");
		EXO_ASSERT(recent->hubs[0].visited > 0);
	}

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::CloseHub{1}));
	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::ClearRecentHubs{}));
	EXO_ASSERT(recorder.waitForEvent<Ev::RecentHubs>([](const Ev::RecentHubs& e) { return e.hubs.empty(); }));

	running.onCore([&] { subscription.reset(); });
	return 1;
});

EXO_TEST(service_a_favourite_is_kept_and_let_go,
{
	namespace Ev = QuickDC::Api::Events;

	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::AddFavouriteHub{"hub.example.org"}));
	EXO_ASSERT(recorder.waitForEvent<Ev::FavouriteHubs>([](const Ev::FavouriteHubs& e) {
		return e.urls.size() == 1 && e.urls[0] == "dchub://hub.example.org:411";
	}));

	std::string stored;
	running.onCore([&] { stored = running.core->config->peekString("Favourites", "Hub 0", ""); });
	EXO_ASSERT(stored == "dchub://hub.example.org:411");

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::RemoveFavouriteHub{"dchub://hub.example.org"}));
	EXO_ASSERT(recorder.waitForEvent<Ev::FavouriteHubs>([](const Ev::FavouriteHubs& e) { return e.urls.empty(); }));

	running.onCore([&] { subscription.reset(); });
	return 1;
});

/* A hub's own board choices are kept in its session group, and following the general setting keeps none. */
EXO_TEST(service_a_hubs_board_choices_are_kept_in_its_group,
{
	namespace Ev = QuickDC::Api::Events;
	using Policy = QuickDC::Api::Commands::SetBoardPolicy;

	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	EXO_ASSERT(running.core->submit(Policy{"adc://127.0.0.1:1/", Policy::Policy::Serve, Policy::Choice::Never}));
	EXO_ASSERT(running.core->submit(Policy{"127.0.0.1:1", Policy::Policy::FetchAhead, Policy::Choice::Always}));
	EXO_ASSERT(recorder.waitForEvent<Ev::SettingChanged>([](const Ev::SettingChanged& e) {
		return e.key == "Fetch a thread ahead" && e.value == "true";
	}));

	std::string serve;
	std::string ahead;
	running.onCore([&] {
		serve = running.core->session->peekString("adc://127.0.0.1:1", "Offer what I have read", "");
		ahead = running.core->session->peekString("dchub://127.0.0.1:1", "Fetch a thread ahead", "");
	});
	EXO_ASSERT(serve == "false");
	EXO_ASSERT(ahead == "true");

	EXO_ASSERT(running.core->submit(Policy{"adc://127.0.0.1:1", Policy::Policy::Serve, Policy::Choice::Follow}));
	EXO_ASSERT(recorder.waitForEvent<Ev::SettingChanged>([](const Ev::SettingChanged& e) {
		return e.key == "Offer what I have read" && e.removed;
	}));

	running.onCore([&] { subscription.reset(); });
	return 1;
});

/* The directory's sources are the core's, told on ReportState and on each change, a failed fetch with its reason. */
EXO_TEST(service_the_hub_directory_is_announced,
{
	namespace Ev = QuickDC::Api::Events;

	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::ResetHubListSources{}));
	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::ReportState{7}));
	EXO_ASSERT(recorder.waitForEvent<Ev::HubListSources>([](const Ev::HubListSources& e) {
		return e.request == 7 && e.sources.size() == 3 && e.sources[0].url == "https://dchublist.org/hublist.xml.bz2";
	}));
	EXO_ASSERT(recorder.waitForEvent<Ev::HubDirectory>([](const Ev::HubDirectory& e) {
		return e.request == 7 && e.hubs.empty();
	}));

	const std::string down = "http://127.0.0.1:1/hublist.xml";
	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::AddHubListSource{down}));
	const std::optional<Ev::HubListSources> failed = recorder.waitForEvent<Ev::HubListSources>(
		[&](const Ev::HubListSources& e) {
			return e.sources.size() == 4 && e.sources[3].url == down && !e.sources[3].error.empty();
		});
	EXO_ASSERT(failed.has_value());
	if (failed) EXO_ASSERT(!failed->sources[3].fetching);

	std::string stored;
	running.onCore([&] { stored = running.core->config->peekString("Hublists", "List 3", ""); });
	EXO_ASSERT(stored == down);

	bool told = false;
	running.onCore([&] {
		for (const QuickDC::Api::Event& event : running.core->state())
			if (const auto* sources = event.as<Ev::HubListSources>()) told = sources->sources.size() == 4;
	});
	EXO_ASSERT(told);

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::RemoveHubListSource{down}));
	EXO_ASSERT(recorder.waitForEvent<Ev::HubListSources>([](const Ev::HubListSources& e) {
		return e.request == 0 && e.sources.size() == 3;
	}));

	running.onCore([&] { subscription.reset(); });
	return 1;
});

/* The core numbers each search and says which request it was; the front end invents nothing. */
EXO_TEST(service_each_search_is_numbered_by_the_core,
{
	namespace Ev = QuickDC::Api::Events;
	using Search = QuickDC::Api::Commands::Search;

	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	Search first;
	first.request = 7;
	first.include = {"one"};
	Search second;
	second.request = 8;
	second.include = {"two"};
	EXO_ASSERT(running.core->submit(first));
	EXO_ASSERT(running.core->submit(second));

	const auto a = recorder.waitForEvent<Ev::SearchSent>([](const Ev::SearchSent& e) { return e.request == 7; });
	const auto b = recorder.waitForEvent<Ev::SearchSent>([](const Ev::SearchSent& e) { return e.request == 8; });
	EXO_ASSERT(a && b);
	if (a && b)
	{
		EXO_ASSERT(a->search != 0 && b->search != 0 && a->search != b->search);
		EXO_ASSERT(a->hubsAsked == 0);
	}

	running.onCore([&] { subscription.reset(); });
	return 1;
});

/* What the core is and listens on, told at its first turn without anybody asking. */
EXO_TEST(service_the_core_tells_its_status,
{
	namespace Ev = QuickDC::Api::Events;

	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::ReportStatus{3}));
	const auto status = recorder.waitForEvent<Ev::CoreStatus>([](const Ev::CoreStatus& e) { return e.request == 3; });
	EXO_ASSERT(status.has_value());
	if (status)
	{
		EXO_ASSERT(!status->version.empty());
		EXO_ASSERT(status->started > 0);
		/* This core is built without a peer port or a hub. */
		EXO_ASSERT(status->port == 0 && !status->tcp && !status->localHub);
	}

	bool told = false;
	running.onCore([&] {
		for (const QuickDC::Api::Event& event : running.core->state())
			if (event.as<Ev::CoreStatus>()) told = true;
	});
	EXO_ASSERT(told);

	running.onCore([&] { subscription.reset(); });
	return 1;
});

/* What a core without a hub, or without the session asked about, cannot do is said rather than dropped. */
EXO_TEST(service_operator_commands_with_nothing_to_act_on_are_refused,
{
	namespace Ev = QuickDC::Api::Events;
	namespace Cmd = QuickDC::Api::Commands;

	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	EXO_ASSERT(running.core->submit(Cmd::KickUser{9, "AAAA", "bye", "", 0}));
	const auto kick = recorder.waitForEvent<Ev::KickRefused>([](const Ev::KickRefused& e) { return e.hub == 9; });
	EXO_ASSERT(kick && kick->why == Ev::KickRefused::Why::NoSession && kick->userId == "AAAA");

	EXO_ASSERT(running.core->submit(Cmd::BanFromLocalHub{4, "carol", "", 0}));
	const auto ban = recorder.waitForEvent<Ev::LocalHubRefused>([](const Ev::LocalHubRefused& e) { return e.request == 4; });
	EXO_ASSERT(ban && ban->why == Ev::LocalHubRefused::Why::NoHub);

	EXO_ASSERT(running.core->submit(Cmd::FindShared{5, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}));
	const auto shared = recorder.waitForEvent<Ev::SharedFile>([](const Ev::SharedFile& e) { return e.request == 5; });
	EXO_ASSERT(shared && !shared->found);

	EXO_ASSERT(running.core->submit(Cmd::SnapshotConnections{6}));
	EXO_ASSERT(recorder.waitForEvent<Ev::ConnectionsSnapshot>(
		[](const Ev::ConnectionsSnapshot& e) { return e.request == 6; }));

	running.onCore([&] { subscription.reset(); });
	return 1;
});

/* A core of its own home, on a port the system picks, administering the hub it runs. */
EXO_TEST(service_the_local_hub_is_administered_through_commands,
{
	namespace Ev = QuickDC::Api::Events;
	namespace Cmd = QuickDC::Api::Commands;
	using Credential = QuickDC::Api::LocalHubCredential;

	std::string home = "/tmp/quickdc_service_home_XXXXXX";
	EXO_ASSERT(mkdtemp(home.data()) != nullptr);

	QuickDC::Core::Home where;
	where.directory = home;
	where.settings["Network"] = {{"Mode", "advanced"}, {"Bind", "127.0.0.1"}, {"Bind6", ""}, {"Port", "41411"}};
	where.settings["Local hub"] = {{"Enabled", "1"}};

	{
		QuickDC::Core core(where);
		Recorder recorder;
		QuickDC::Subscription subscription = core.subscribe(&recorder);

		std::thread loop([&] { core.runUntilStopped(); });

		const auto up = recorder.waitForEvent<Ev::CoreStatus>([](const Ev::CoreStatus&) { return true; });
		EXO_ASSERT(up && up->port != 0 && up->tcp && up->localHub);

		EXO_ASSERT(core.submit(Cmd::SetLocalHubAccount{1, "oper", "secret", Credential::Operator}));
		const auto account = recorder.waitForEvent<Ev::LocalHubState>([](const Ev::LocalHubState& e) { return e.request == 1; });
		EXO_ASSERT(account && account->enabled && account->accounts.size() == 1);
		if (account && !account->accounts.empty())
		{
			EXO_ASSERT(account->accounts[0].nick == "oper");
			EXO_ASSERT(account->accounts[0].credential == Credential::Operator);
		}

		EXO_ASSERT(core.submit(Cmd::BanFromLocalHub{2, "10.0.0.1", "spam", 60}));
		const auto banned = recorder.waitForEvent<Ev::LocalHubState>([](const Ev::LocalHubState& e) { return e.request == 2; });
		EXO_ASSERT(banned && banned->bans.size() == 1);
		if (banned && !banned->bans.empty())
		{
			EXO_ASSERT(banned->bans[0].address == "10.0.0.1" && banned->bans[0].reason == "spam");
			EXO_ASSERT(banned->bans[0].until > 0);
		}

		EXO_ASSERT(core.submit(Cmd::UnbanFromLocalHub{3, "10.0.0.1"}));
		EXO_ASSERT(recorder.waitForEvent<Ev::LocalHubState>(
			[](const Ev::LocalHubState& e) { return e.request == 3 && e.bans.empty(); }));
		EXO_ASSERT(core.submit(Cmd::UnbanFromLocalHub{4, "10.0.0.1"}));
		const auto none = recorder.waitForEvent<Ev::LocalHubRefused>([](const Ev::LocalHubRefused& e) { return e.request == 4; });
		EXO_ASSERT(none && none->why == Ev::LocalHubRefused::Why::NoSuchEntry);

		EXO_ASSERT(core.submit(Cmd::KickFromLocalHub{5, "nobody", ""}));
		const auto nobody = recorder.waitForEvent<Ev::LocalHubRefused>([](const Ev::LocalHubRefused& e) { return e.request == 5; });
		EXO_ASSERT(nobody && nobody->why == Ev::LocalHubRefused::Why::NoSuchUser);

		/* Turned off, the hub stops taking logins and the status says so. */
		EXO_ASSERT(core.submit(Cmd::SetSetting{QuickDC::Api::SettingsStore::Application, "Local hub", "Enabled", "0", false, 6}));
		EXO_ASSERT(recorder.waitForEvent<Ev::CoreStatus>([](const Ev::CoreStatus& e) { return e.tcp && !e.localHub; }));

		core.requestStop();
		loop.join();
		subscription.reset();
	}

	std::error_code ignored;
	std::filesystem::remove_all(home, ignored);
	return 1;
});
