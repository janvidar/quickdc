/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "config/preferences.h"
#include <time.h>
#include <map>
#include <string>

namespace QuickDC {

Preferences::Preferences(const std::string& file) : IniParser(file) {
	read();
	current = "Default";
	dirty = false;
}

Preferences::~Preferences() {
	if (dirty) write();
}

void Preferences::setGroup(const std::string& group) {
	current = group;
}

int Preferences::getNumber(const std::string& key, const int defval) {
	group settings = groups[current];
	if (settings.find(key) != settings.end()) {
		std::string val = settings[key];
		return quickdc_atoi(val.c_str());
	} else {
		setNumber(key, defval);
		return defval;
	}
}

const char* Preferences::getString(const std::string& key, const std::string& defval) {
	group settings = groups[current];
	if (settings.find(key) != settings.end()) {
		return settings[key].c_str();
	} else {
		setString(key, defval);
		return defval.c_str();
	}

}



bool Preferences::getBool(const std::string& key, const bool defval) {
	group settings = groups[current];
	if (settings.find(key) != settings.end()) {
		std::string val = settings[key];
		return ((val == "true") || (val == "1"));
	} else {
		setBool(key, defval);
		return defval;
	}
}

void Preferences::setNumber(const std::string& key, int val) {
	group settings = groups[current];
	settings[key] = std::string(quickdc_itoa(val, 10));
	groups[current] = settings;
	dirty = true;
}

void Preferences::setString(const std::string& key, const std::string& val) {
	group settings = groups[current];
	settings[key] = val;
	groups[current] = settings;
	dirty = true;
}

void Preferences::setBool(const std::string& key, bool val) {
	group settings = groups[current];
	settings[key] = std::string(val ? "true" : "false");
	groups[current] = settings;
	dirty = true;
}

void Preferences::setTime(const std::string& key, const Samurai::TimeStamp& time) {
	group settings = groups[current];
	settings[key] = std::string(quickdc_itoa(time.getInternalData(), 10));
	groups[current] = settings;
	dirty = true;
}

Samurai::TimeStamp Preferences::getTime(const std::string& key) {
	group settings = groups[current];
	if (settings.find(key) != settings.end()) {
		std::string val = settings[key];
		Samurai::TimeStamp now = Samurai::TimeStamp(quickdc_atoi(val.c_str()));
		return now;
	} else {
		Samurai::TimeStamp now;
		setTime(key, now);
		return now;
	}
}


void Preferences::remove(const std::string& key) {
	group settings = groups[current];
	if (settings.find(key) != settings.end()) {
		settings.erase(key);
	}
}

bool Preferences::haveSetting(const std::string& key) {
	group settings = groups[current];
	return (settings.find(key) != settings.end());
}

bool Preferences::read() {
	if (!IniParser::read()) return false;
	return true;
}


bool Preferences::write() {
	if (!dirty) return true;

	writeStart();

	std::map<const std::string, group>::iterator group_iterator;
	std::map<const std::string, std::string>::iterator prefs_iterator;
	for (group_iterator = groups.begin(); group_iterator != groups.end(); group_iterator++) {
		writeSectionStart((char*) (*group_iterator).first.c_str());
		for (prefs_iterator = (*group_iterator).second.begin(); prefs_iterator != (*group_iterator).second.end(); prefs_iterator++) {
			writeSetting(((char*) (*prefs_iterator).first.c_str()), (char*) (*prefs_iterator).second.c_str());
		}
		writeSectionEnd((char*) (*group_iterator).first.c_str());
	}

	writeEnd();
	dirty = false;
	return IniParser::write();
}



void Preferences::readSectionStart(const std::string& group) {
	setGroup(group);
}

void Preferences::readSectionEnd(const std::string&) {
	setGroup("Default");
}

void Preferences::readSetting(const std::string& name, const std::string& value) {
	QDBG("Preferences::readSetting(\"%s\", \"%s\")", name.c_str(), value.c_str());
	setString(name, value);
}

}
