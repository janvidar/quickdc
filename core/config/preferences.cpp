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
	/* Named before the read, or a setting appearing before the first section
	   header lands in a group with no name - which is written back out as "[]"
	   and refused the next time the file is read. */
	current = "Default";
	read();
	dirty = false;
}

Preferences::~Preferences() {
	if (dirty) write();
}

void Preferences::setGroup(const std::string& group) {
	current = group;
}

int Preferences::getNumber(const std::string& key, const int defval) {
	group& settings = groups[current];
	if (settings.find(key) != settings.end()) {
		std::string val = settings[key];
		return Samurai::Util::Convert::to_int32(val.c_str());
	} else {
		setNumber(key, defval);
		return defval;
	}
}

/*
 * NOTE: the returned pointer refers to the stored value, which lives as long
 * as the setting is not overwritten or removed. Never return defval.c_str()
 * here -- defval is typically bound to a temporary that dies at the end of the
 * caller's full-expression.
 */
const char* Preferences::getString(const std::string& key, const std::string& defval) {
	group& settings = groups[current];
	if (settings.find(key) == settings.end()) {
		setString(key, defval);
	}
	return settings[key].c_str();
}



bool Preferences::getBool(const std::string& key, const bool defval) {
	group& settings = groups[current];
	if (settings.find(key) != settings.end()) {
		std::string val = settings[key];
		return ((val == "true") || (val == "1"));
	} else {
		setBool(key, defval);
		return defval;
	}
}

void Preferences::setNumber(const std::string& key, int val) {
	groups[current][key] = std::to_string(val);
	dirty = true;
}

void Preferences::setString(const std::string& key, const std::string& val) {
	groups[current][key] = val;
	dirty = true;
}

void Preferences::setBool(const std::string& key, bool val) {
	groups[current][key] = std::string(val ? "true" : "false");
	dirty = true;
}

void Preferences::setTime(const std::string& key, const Samurai::TimeStamp& time) {
	groups[current][key] = std::to_string(time.getInternalData());
	dirty = true;
}

Samurai::TimeStamp Preferences::getTime(const std::string& key) {
	group& settings = groups[current];
	if (settings.find(key) != settings.end()) {
		std::string val = settings[key];
		Samurai::TimeStamp now = Samurai::TimeStamp(Samurai::Util::Convert::to_int32(val.c_str()));
		return now;
	} else {
		Samurai::TimeStamp now;
		setTime(key, now);
		return now;
	}
}


void Preferences::remove(const std::string& key) {
	group& settings = groups[current];
	if (settings.erase(key))
		dirty = true;
}

bool Preferences::haveSetting(const std::string& key) {
	group& settings = groups[current];
	return (settings.find(key) != settings.end());
}

const std::string* Preferences::find(const std::string& group, const std::string& key) const {
	auto section = groups.find(group);
	if (section == groups.end()) return nullptr;

	auto setting = section->second.find(key);
	if (setting == section->second.end()) return nullptr;

	return &setting->second;
}

std::vector<std::string> Preferences::groupNames() const {
	std::vector<std::string> names;
	names.reserve(groups.size());

	for (const auto& section : groups)
		names.push_back(section.first);

	return names;
}

std::vector<std::string> Preferences::keysIn(const std::string& group) const {
	std::vector<std::string> keys;

	auto section = groups.find(group);
	if (section == groups.end()) return keys;

	keys.reserve(section->second.size());
	for (const auto& setting : section->second)
		keys.push_back(setting.first);

	return keys;
}

void Preferences::setNumber(const std::string& group, const std::string& key, int val) {
	groups[group][key] = std::to_string(val);
	dirty = true;
}

void Preferences::setString(const std::string& group, const std::string& key, const std::string& val) {
	groups[group][key] = val;
	dirty = true;
}

void Preferences::setBool(const std::string& group, const std::string& key, bool val) {
	groups[group][key] = std::string(val ? "true" : "false");
	dirty = true;
}

void Preferences::remove(const std::string& group, const std::string& key) {
	auto section = groups.find(group);
	if (section == groups.end()) return;

	if (section->second.erase(key))
		dirty = true;
}

bool Preferences::haveSetting(const std::string& group, const std::string& key) const {
	return find(group, key) != nullptr;
}

int Preferences::peekNumber(const std::string& group, const std::string& key, int defval) const {
	const std::string* val = find(group, key);
	return val ? Samurai::Util::Convert::to_int32(val->c_str()) : defval;
}

std::string Preferences::peekString(const std::string& group, const std::string& key, const std::string& defval) const {
	const std::string* val = find(group, key);
	return val ? *val : defval;
}

bool Preferences::peekBool(const std::string& group, const std::string& key, bool defval) const {
	const std::string* val = find(group, key);
	if (!val) return defval;

	return ((*val == "true") || (*val == "1"));
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
		writeSectionStart((*group_iterator).first.c_str());
		for (prefs_iterator = (*group_iterator).second.begin(); prefs_iterator != (*group_iterator).second.end(); prefs_iterator++) {
			writeSetting((*prefs_iterator).first.c_str(), (*prefs_iterator).second.c_str());
		}
		writeSectionEnd((*group_iterator).first.c_str());
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
