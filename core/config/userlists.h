/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONFIG_USERLISTS_H
#define HAVE_QUICKDC_CONFIG_USERLISTS_H

#include <cstdint>
#include <string>
#include <vector>

namespace QuickDC {

class Preferences;

/*
 * The lists a user keeps of where they have been and what they kept: the hubs
 * they chose to keep, the hubs they visited, and what finished downloading.
 *
 * Each is held in memory, written through to the configuration on every change
 * and the file saved, in the numbered shape the configuration uses for [Shares].
 * Addresses are kept as canonicalHubUrl() spells them, so a hub reached by two
 * spellings is one entry.
 */

/**
 *     [Favourites]
 *     Total=2
 *     Hub 0=adcs://hub.example.org:412
 *     Hub 1=dchub://other.example.org:411
 *
 * In the order they were added. A list written before addresses were canonical
 * is rewritten as it is read, and two spellings of one hub become one entry.
 */
class FavouriteHubs {
	public:
		explicit FavouriteHubs(Preferences& config);

		const std::vector<std::string>& urls() const { return kept; }
		bool contains(const std::string& url) const;

		/** @return whether the list changed. */
		bool add(const std::string& url);
		bool remove(const std::string& url);

	private:
		void store();

		Preferences& config;
		std::vector<std::string> kept;
		int stored = 0;
};

struct RecentHub {
	std::string url;
	/** What the hub called itself, where it has said. */
	std::string name;
	/** Seconds since the epoch. */
	uint64_t visited = 0;
};

/**
 *     [Recent hubs]
 *     Total=1
 *     Hub 0=1769000000|adcs://hub.example.org:412|The Example Hub
 *
 * When it was last visited, its address, and its name - last and unescaped, since
 * a hub may call itself anything. Newest first and bounded: a way back to
 * somewhere, not a history. A visit is recorded when a hub is opened rather than
 * when its login succeeds, because a hub that refused is one somebody may well
 * want to find again to see why.
 */
class RecentHubs {
	public:
		static constexpr int LIMIT = 32;

		explicit RecentHubs(Preferences& config);

		const std::vector<RecentHub>& entries() const { return hubs; }

		/** A visit, moving the hub to the front; the name it had is kept. */
		void visited(const std::string& url, uint64_t now);

		/** What a hub calls itself, without counting as a visit. @return whether it changed. */
		bool named(const std::string& url, const std::string& name);

		bool remove(const std::string& url);
		bool clear();

	private:
		void store();

		Preferences& config;
		std::vector<RecentHub> hubs;
		int stored = 0;
};

struct CompletedDownload {
	std::string filename;
	std::string tth;
	uint64_t size = 0;
	/** Seconds since the epoch. */
	uint64_t finished = 0;
};

/**
 *     [Completed]
 *     Total=1
 *     Entry 0=1769000000|734003200|MWXFLX…|/home/user/downloads/film.mkv
 *
 * When it finished, how big it was, its hash, and where it went - last and
 * unescaped, since a path may hold the separator. Newest first and bounded.
 */
class CompletedDownloads {
	public:
		static constexpr int LIMIT = 500;

		/** Closer than this, the same name is one completion told twice. */
		static constexpr uint64_t REPEAT_WINDOW_SECONDS = 5;

		explicit CompletedDownloads(Preferences& config);

		const std::vector<CompletedDownload>& entries() const { return done; }

		/** @return false where it was a repeat, or names no file. */
		bool add(const CompletedDownload& entry);

		/** Forget one by the file it went to; the file itself is left alone. */
		bool remove(const std::string& filename);
		bool clear();

	private:
		void store();

		Preferences& config;
		std::vector<CompletedDownload> done;
		int stored = 0;
};

}

#endif // HAVE_QUICKDC_CONFIG_USERLISTS_H
