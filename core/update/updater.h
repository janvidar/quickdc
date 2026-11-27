/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_UPDATE_UPDATER_H
#define HAVE_QUICKDC_UPDATE_UPDATER_H

#include "api/protocol.h"
#include "update/feed.h"

#include <samurai/io/net/http/client.h>
#include <samurai/io/net/url.h>
#include <samurai/timer.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace QuickDC {

class Preferences;

namespace Update {

/** The package platform this build is, as feeds name it; empty where there are no packages. */
const char* platformName();

/** The channel this build was packaged for; empty for one built by hand. */
const char* buildChannel();

/**
 * Asks the channel's feed whether there is something newer, fetches it, and
 * checks it is what the feed signed for. Installing it is left to the quit.
 *
 * Silent: a check a few minutes after start and about daily after that, nothing
 * said unless something is found or goes wrong. Each check reads the settings
 * afresh, so turning updates off takes effect at the next one.
 */
class Updater final : private Samurai::IO::Net::HTTP::RequestEventHandler,
                      private Samurai::TimerListener
{
	public:
		using Publish = std::function<void(const Api::Event&)>;

		/**
		 * @param home    where the updates directory is made; empty for none,
		 *                which never checks.
		 * @param running the version this build is (VERSION).
		 */
		Updater(Samurai::EventLoop& loop, const Preferences& config, std::string home,
		        std::string running, Publish publish);
		~Updater() override;

		Updater(const Updater&) = delete;
		Updater& operator=(const Updater&) = delete;

		/** Check now, as asked by a person: a reason not to is said rather than kept quiet. */
		void check();

		/** Where packages are kept. */
		std::string directory() const;

	private:
		enum class Stage { Idle, Feed, Package };

		void begin(bool asked);
		void fail(Api::Events::UpdateFailed::Why why, std::string reason);
		void finish();
		void schedule(std::chrono::milliseconds after);
		void fetch(const Samurai::IO::Net::HTTP::Client::Options& options, const Samurai::IO::Net::URL& url);

		/** Whether 'url' may be fetched for 'channel': https, or a test channel on the loopback. */
		bool allowed(const std::string& url) const;

		/** Why 'url' is not allowed(), said of 'what'. */
		std::string refusal(const char* what, const std::string& url) const;

		void feedArrived(const std::string& body);
		void packageArrived(const std::string& body);

		/** Where the package 'named' in a feed is: as given, or beside the feed; empty for nowhere. */
		std::string packageUrl(const std::string& named) const;

		/** The file the package at 'url' is kept as, or empty for one of no kind this installs. */
		std::string packagePath(const Feed& feed, const std::string& url) const;

		/** Whether 'path' holds exactly what 'feed' signed for. */
		static bool holds(const std::string& path, const Feed& feed);

		/** Everything in the updates directory but 'keep'. */
		void sweep(const std::string& keep) const;

		void EventHttpResponse(Samurai::IO::Net::HTTP::Client*,
		                       const Samurai::IO::Net::HTTP::Response& response) override;
		void EventHttpError(Samurai::IO::Net::HTTP::Client*, Samurai::IO::Net::HTTP::Status,
		                    const char* msg) override;
		void EventTimeout(Samurai::Timer* timer) override;

		Samurai::EventLoop& loop;
		const Preferences& config;
		const std::string home;
		const std::string running;
		const Publish publish;

		std::unique_ptr<Samurai::IO::Net::HTTP::Client> client;
		std::unique_ptr<Samurai::Timer> timer;
		/* The one replaced last, which may still be delivering the callback that replaced it. */
		std::unique_ptr<Samurai::IO::Net::HTTP::Client> retiredClient;
		std::unique_ptr<Samurai::Timer> retiredTimer;

		Stage stage = Stage::Idle;
		std::string channel;
		std::optional<Feed> offer;
		std::string offerPath;
		/** The channel's directory on the feed server, ending in a slash. */
		std::string feedDirectory;
};

}
}

#endif // HAVE_QUICKDC_UPDATE_UPDATER_H
