/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONFIG_PREFERENCES_H
#define HAVE_QUICKDC_CONFIG_PREFERENCES_H

#include "iniparser.h"
#include <map>
#include <string>
#include <vector>
#include <samurai/timestamp.h>

namespace QuickDC {

class Preferences : public IniParser {
	public:
		typedef std::map<const std::string, std::string> group;
		typedef std::map<const std::string, group> groupmap;

		Preferences(const std::string& file);
		virtual ~Preferences();

		// groups
		std::string getGroup() const { return current; }
		void setGroup(const std::string& group);

		/**
		 * Everything stored, group by group.
		 *
		 * For a reader that wants the lot rather than a key at a time - a user
		 * interface holding its own copy so that drawing never has to ask. Taken
		 * in one pass, so nothing can select another group half way through it.
		 */
		const groupmap& snapshot() const { return groups; }

		std::vector<std::string> groupNames() const;

		/** The keys in a group, or nothing at all if there is no such group. */
		std::vector<std::string> keysIn(const std::string& group) const;

		// settings
		int getNumber(const std::string& key, const int defval = 0);
		const char* getString(const std::string& key, const std::string& defval = "");
		bool getBool(const std::string& key, const bool defval = false);
		void setNumber(const std::string& key, int val);
		void setTime(const std::string& key, const Samurai::TimeStamp& time);
		Samurai::TimeStamp getTime(const std::string& key);
		void setString(const std::string& key, const std::string& val);
		void setBool(const std::string& key, bool val);
		void remove(const std::string& key);
		bool haveSetting(const std::string& key);

		/**
		 * The same settings, each named with the group it is in.
		 *
		 * The current group is shared mutable state, so reading a key means
		 * selecting a group and then reading, and anything at all in between
		 * reads from the wrong section. Naming the group on the call removes the
		 * gap, and with it the rule that the two have to happen together.
		 */
		void setNumber(const std::string& group, const std::string& key, int val);
		void setString(const std::string& group, const std::string& key,
		               const std::string& val);
		void setBool(const std::string& group, const std::string& key, bool val);
		void remove(const std::string& group, const std::string& key);
		bool haveSetting(const std::string& group, const std::string& key) const;

		/**
		 * Read without writing.
		 *
		 * The getters above store the default when a key is absent, so merely
		 * asking after a setting nobody has ever set marks the file dirty and
		 * earns it a rewrite. These answer the same question and leave the store
		 * as they found it, which is what a reader that is only looking wants.
		 */
		int peekNumber(const std::string& group, const std::string& key,
		               int defval) const;
		std::string peekString(const std::string& group, const std::string& key,
		                       const std::string& defval) const;
		bool peekBool(const std::string& group, const std::string& key,
		              bool defval) const;

		virtual bool write() override;
		virtual bool read() override;

	protected:
		void readSectionStart(const std::string& group) override;
		void readSectionEnd(const std::string& group) override;
		void readSetting(const std::string& name, const std::string& value) override;

	protected:
		/** The stored value, or null where there is no such group or key. */
		const std::string* find(const std::string& group,
		                        const std::string& key) const;

	protected:
		bool dirty;

		// groups
		std::string current;
		groupmap groups;
};

}

#endif // HAVE_QUICKDC_CONFIG_PREFERENCES_H
