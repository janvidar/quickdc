/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONFIG_PREFERENCES_H
#define HAVE_QUICKDC_CONFIG_PREFERENCES_H

#include "iniparser.h"
#include <map>
#include <string>
#include <samurai/timestamp.h>

namespace QuickDC {

class Preferences : public IniParser {
	public:
		Preferences(const std::string& file);
		virtual ~Preferences();
		
		// groups
		std::string getGroup() const { return current; }
		void setGroup(const std::string& group);

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

		virtual bool write();
		virtual bool read();

	protected:
		void readSectionStart(const std::string& group);
		void readSectionEnd(const std::string& group);
		void readSetting(const std::string& name, const std::string& value);

	protected:
		bool dirty;

		// groups
		std::string current;
		typedef std::map<const std::string, std::string> group;
		std::map<const std::string, group> groups;
};

}

#endif // HAVE_QUICKDC_CONFIG_PREFERENCES_H
