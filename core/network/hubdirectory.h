/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HUBDIRECTORY_H
#define HAVE_QUICKDC_HUBDIRECTORY_H

#include "api/protocol.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Samurai { class EventLoop; }

namespace QuickDC {

class Preferences;

/**
 * The public hubs, from several lists merged into one.
 *
 * There is no single directory of DC hubs and never was: each hublist is one
 * server's idea of what exists, they disagree, and any one of them may be down for
 * a week. So several are read and folded together, in the order they are
 * configured - which makes that order a precedence, since where two lists describe
 * the same hub the earlier one's description is the one kept.
 *
 * The sources are configuration, as
 *
 *     [Hublists]
 *     Total=2
 *     List 0=https://dchublist.org/hublist.xml.bz2
 *     List 1=https://te-home.net/?do=hublist&get=hublist.xml.bz2
 *
 * written out with the defaults the first time so that they can be edited.
 *
 * Each source's list is cached, one file per source under the cache directory,
 * so a directory is shown at once rather than after three HTTP requests. Nothing
 * is fetched until refresh() is asked; a cache older than MAX_AGE_HOURS is then
 * fetched again, and a cache that is merely old is still shown, because a week old
 * list of hubs is worth far more than no list at all.
 */
class HubDirectory {
	public:
		static constexpr int MAX_AGE_HOURS = 12;

		/**
		 * How long a source that has just been asked for is left alone, so one that
		 * is down is tried once rather than every time the directory is looked at.
		 * Not persisted: once per run.
		 */
		static constexpr int RETRY_MINUTES = 15;

		static constexpr int SOURCE_LIMIT = 32;

		/*
		 * The core already refuses a list naming more than 65536 hubs. These bound
		 * what one source contributes and what the merge holds. The public lists
		 * carry a couple of thousand hubs each.
		 */
		static constexpr size_t PER_SOURCE_LIMIT = 20000;
		static constexpr size_t MERGED_LIMIT = 65536;

		/**
		 * @param cacheDir where the per-source caches are kept; empty for none.
		 * @param sourcesChanged told when any source's state changes.
		 * @param hubsChanged told when the merged list changes.
		 */
		HubDirectory(Samurai::EventLoop& loop, Preferences& config, std::string cacheDir,
		             std::function<void()> sourcesChanged, std::function<void()> hubsChanged);
		~HubDirectory();

		HubDirectory(const HubDirectory&) = delete;
		HubDirectory& operator=(const HubDirectory&) = delete;

		/** In precedence order. */
		std::vector<Api::HubListSource> sources() const;

		const std::vector<Api::DirectoryHub>& hubs() const { return merged; }

		/** The source addresses DirectoryHub::listedBy indexes. */
		const std::vector<std::string>& mergedFrom() const { return mergedSources; }

		/** Fetch what is due at 'now' (seconds since the epoch), or every source with 'force'. */
		void refresh(bool force, uint64_t now);

		/** @return whether the sources changed. A source already listed is not added again. */
		bool add(const std::string& url, uint64_t now);
		bool remove(const std::string& url);

		/** Back to defaultSources(). @return whether that changed anything. */
		bool reset();

		/** The lists read when the configuration names none. */
		static std::vector<std::string> defaultSources();

		/**
		 * Lists this once shipped as a default and no longer does, dropped from a
		 * stored configuration as well as from the defaults: each was written there
		 * by this program rather than chosen, so withdrawing one while leaving it in
		 * the file would fix nothing for anybody who had started the program once.
		 * Matched exactly; a list somebody added themselves is theirs.
		 */
		static std::vector<std::string> retiredSources();

		/** Where the cache for 'url' is kept under 'cacheDir'. */
		static std::string cacheFile(const std::string& cacheDir, const std::string& url);

	private:
		class Fetch;

		struct Source {
			std::string url;
			uint64_t fetched = 0;
			uint64_t attempted = 0;
			bool cached = false;
			bool fetching = false;
			std::string error;
			std::vector<HubListEntry> hubs;
		};

		void load();
		void store();
		void readCache(Source& source) const;
		void writeCache(const Source& source) const;
		void merge();

		/** Whether a fetch of 'source' is due at 'now'. */
		static bool due(const Source& source, uint64_t now);
		void fetch(Source& source, uint64_t now);

		void fetched(const std::string& url, const std::vector<HubListEntry>& hubs);
		void failed(const std::string& url, const std::string& reason);

		Source* find(const std::string& url);

		Samurai::EventLoop& loop;
		Preferences& config;
		std::string cacheDir;
		std::function<void()> sourcesChanged;
		std::function<void()> hubsChanged;

		std::vector<Source> list;
		int stored = 0;

		/* By address, apart from the sources, so one removed mid-fetch can be let go. */
		std::map<std::string, std::unique_ptr<Fetch>> fetches;

		std::vector<std::string> mergedSources;
		std::vector<Api::DirectoryHub> merged;
};

}

#endif // HAVE_QUICKDC_HUBDIRECTORY_H
