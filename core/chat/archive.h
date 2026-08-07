/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CHATARCHIVE_H
#define HAVE_QUICKDC_CHATARCHIVE_H

#include "quickdc.h"

#include <stdint.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace QuickDC {

/**
 * What was said on a hub, kept so a conversation carries on across a restart.
 *
 * One file per conversation, appended to a line at a time and never rewritten.
 * A transcript only grows at its end, and an append is the one write that leaves
 * a reader either the whole of the old file or the whole of the new one - so a
 * client that goes down mid-write costs the last thing somebody said rather than
 * everything they ever said.
 *
 * Nothing is ever dropped. An interface holds the recent end of a conversation
 * and this holds all of it; tail() is where the two meet.
 *
 * Private conversations are kept the same way as public ones, which puts them on
 * disk in the clear. Anything running as this user can read them, which is the
 * same standing as the download queue and the hub passwords beside them - worth
 * saying out loud rather than leaving to be discovered.
 */
class ChatArchive {

	public:
		/** One line of a conversation, as it is written down and read back. */
		struct Line {
			/** Seconds since the epoch, as the message arrived. */
			uint64_t    time = 0;

			/** Who said it. Empty when the hub itself did; see 'hub'. */
			std::string nick;

			/**
			 * What identifies the speaker beyond this session: the client ID on
			 * ADC, the nickname on NMDC. Not the session ID, which names somebody
			 * else entirely the next time they connect.
			 */
			std::string source;

			/** The name a hub gave for a line of its own. Empty for a person. */
			std::string hub;

			std::string text;

			/** They were an operator when they said it; see the note in append(). */
			bool op = false;

			bool action = false;
			bool richText = false;
			bool privateMessage = false;
		};

		/**
		 * @param directory where the transcripts live, "~" resolved as everywhere
		 *        else in the core. Created if it is not there, and every call is a
		 *        no-op if it cannot be.
		 */
		explicit ChatArchive(const std::string& directory);

		ChatArchive(const ChatArchive&) = delete;
		ChatArchive& operator=(const ChatArchive&) = delete;

		/**
		 * Write one line down, unless it is already there.
		 *
		 * The operator flag is stored rather than looked up when it is read back,
		 * because it is a fact about the moment: whoever was an operator when they
		 * spoke stays one in the record, however the hub has since changed its
		 * mind.
		 *
		 * A hub replaying its history sends lines this client may already hold,
		 * stamped with the times they were first said - so a line whose stamp is
		 * no newer than the newest already written is looked for before it is
		 * added. A line newer than everything on file cannot be a repeat of any
		 * of it and is written without looking, which is every line of live chat.
		 *
		 * @param conversation empty for a hub's public chat, and otherwise the
		 *        other party's lasting id - the same identifier as Line::source.
		 *
		 * @return false when the line was already there and nothing was written.
		 *         A caller showing the message to somebody should not: they have
		 *         seen it, and it is on their screen already.
		 */
		bool append(const std::string& hubUrl, const std::string& conversation,
		            const Line& line);

		/**
		 * The last 'limit' lines, oldest first, or fewer if that is all there is.
		 *
		 * Read backwards from the end rather than by walking the file: a
		 * transcript is unbounded and only its recent end is ever wanted, so
		 * reading all of one would make opening a long-running hub cost more the
		 * longer it had been running.
		 */
		std::vector<Line> tail(const std::string& hubUrl,
		                       const std::string& conversation,
		                       size_t limit) const;

		/** Whether anything has been written for this conversation. */
		bool contains(const std::string& hubUrl,
		              const std::string& conversation) const;

		/** Throw a conversation's transcript away. */
		void forget(const std::string& hubUrl, const std::string& conversation);

		/** Where a conversation's transcript is. Public so a test can look. */
		std::string pathFor(const std::string& hubUrl,
		                    const std::string& conversation) const;

		/** Whether the directory could be made; false makes every call do nothing. */
		bool isUsable() const;

	private:
		/** Fills 'seen' and 'newest' for a conversation from what is on disk. */
		void remember(const std::string& path, const std::string& hubUrl,
		              const std::string& conversation) const;

		std::string directory;
		bool usable;

		/**
		 * The recent end of each conversation, by transcript path: what
		 * identifies every line, and the stamp of the newest of them.
		 *
		 * Held rather than read per line, because a replay arrives as a burst and
		 * reading the file back for each one of them would be the whole
		 * transcript read once per message in it.
		 */
		mutable std::map<std::string, std::set<std::string> > seen;
		mutable std::map<std::string, uint64_t> newest;
};

}

#endif // HAVE_QUICKDC_CHATARCHIVE_H
