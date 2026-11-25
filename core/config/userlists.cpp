/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "config/userlists.h"
#include "config/preferences.h"
#include "api/huburl.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace {

const char TOTAL[] = "Total";
const char SEPARATOR = '|';

/* More than anyone keeps, and a bound on a corrupt Total. */
const int FAVOURITES_LIMIT = 4096;

std::string numbered(const char* prefix, int index)
{
	return std::string(prefix) + std::to_string(index);
}

int total(const QuickDC::Preferences& config, const char* group, int limit)
{
	return std::clamp(config.peekNumber(group, TOTAL, 0), 0, limit);
}

/* Write 'lines' as the numbered keys, clearing those a longer list left. */
void writeNumbered(QuickDC::Preferences& config, const char* group, const char* prefix,
                   const std::vector<std::string>& lines, int& stored)
{
	const int count = static_cast<int>(lines.size());
	for (int n = 0; n < count; ++n) config.setString(group, numbered(prefix, n), lines[n]);
	for (int n = count; n < stored; ++n) config.remove(group, numbered(prefix, n));

	config.setNumber(group, TOTAL, count);
	config.write();
	stored = count;
}

std::vector<std::string> split(const std::string& line, size_t parts)
{
	std::vector<std::string> out;
	size_t from = 0;
	while (out.size() + 1 < parts)
	{
		const size_t at = line.find(SEPARATOR, from);
		if (at == std::string::npos) break;
		out.push_back(line.substr(from, at - from));
		from = at + 1;
	}
	out.push_back(line.substr(from));
	return out;
}

bool number(const std::string& text, uint64_t& out)
{
	if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) return false;
	out = std::strtoull(text.c_str(), nullptr, 10);
	return true;
}

}

QuickDC::FavouriteHubs::FavouriteHubs(Preferences& config_)
	: config(config_)
{
	const char* group = "Favourites";
	stored = total(config, group, FAVOURITES_LIMIT);

	bool rewritten = false;
	for (int n = 0; n < stored; ++n)
	{
		const std::string given = config.peekString(group, numbered("Hub ", n), "");
		const std::string url = canonicalHubUrl(given);
		if (url != given) rewritten = true;

		if (url.empty()) continue;
		if (contains(url))
		{
			rewritten = true;
			continue;
		}
		kept.push_back(url);
	}

	if (rewritten) store();
}

bool QuickDC::FavouriteHubs::contains(const std::string& url) const
{
	return std::find(kept.begin(), kept.end(), canonicalHubUrl(url)) != kept.end();
}

bool QuickDC::FavouriteHubs::add(const std::string& url)
{
	const std::string canonical = canonicalHubUrl(url);
	if (canonical.empty() || contains(canonical)) return false;

	kept.push_back(canonical);
	store();
	return true;
}

bool QuickDC::FavouriteHubs::remove(const std::string& url)
{
	const auto found = std::find(kept.begin(), kept.end(), canonicalHubUrl(url));
	if (found == kept.end()) return false;

	kept.erase(found);
	store();
	return true;
}

void QuickDC::FavouriteHubs::store()
{
	writeNumbered(config, "Favourites", "Hub ", kept, stored);
}

QuickDC::RecentHubs::RecentHubs(Preferences& config_)
	: config(config_)
{
	const char* group = "Recent hubs";
	stored = total(config, group, LIMIT);

	for (int n = 0; n < stored; ++n)
	{
		const std::vector<std::string> parts = split(config.peekString(group, numbered("Hub ", n), ""), 3);
		RecentHub entry;
		if (parts.size() < 3 || !number(parts[0], entry.visited) || parts[1].empty()) continue;

		entry.url = parts[1];
		entry.name = parts[2];

		/* The newer line of two naming one hub is the one kept. */
		const bool seen = std::any_of(hubs.begin(), hubs.end(),
			[&entry](const RecentHub& hub) { return hub.url == entry.url; });
		if (!seen) hubs.push_back(std::move(entry));
	}

	std::stable_sort(hubs.begin(), hubs.end(),
		[](const RecentHub& a, const RecentHub& b) { return a.visited > b.visited; });
}

void QuickDC::RecentHubs::visited(const std::string& url, uint64_t now)
{
	RecentHub entry;
	entry.url = canonicalHubUrl(url);
	entry.visited = now;
	if (entry.url.empty()) return;

	const auto found = std::find_if(hubs.begin(), hubs.end(),
		[&entry](const RecentHub& hub) { return hub.url == entry.url; });
	if (found != hubs.end())
	{
		entry.name = found->name;
		hubs.erase(found);
	}

	hubs.insert(hubs.begin(), std::move(entry));
	if (hubs.size() > static_cast<size_t>(LIMIT)) hubs.resize(LIMIT);
	store();
}

bool QuickDC::RecentHubs::named(const std::string& url, const std::string& name)
{
	const std::string wanted = canonicalHubUrl(url);
	for (RecentHub& hub : hubs)
	{
		if (hub.url != wanted) continue;
		if (hub.name == name) return false;

		hub.name = name;
		store();
		return true;
	}
	return false;
}

bool QuickDC::RecentHubs::remove(const std::string& url)
{
	const std::string wanted = canonicalHubUrl(url);
	const auto found = std::find_if(hubs.begin(), hubs.end(),
		[&wanted](const RecentHub& hub) { return hub.url == wanted; });
	if (found == hubs.end()) return false;

	hubs.erase(found);
	store();
	return true;
}

bool QuickDC::RecentHubs::clear()
{
	if (hubs.empty()) return false;
	hubs.clear();
	store();
	return true;
}

void QuickDC::RecentHubs::store()
{
	std::vector<std::string> lines;
	for (const RecentHub& hub : hubs)
		lines.push_back(std::to_string(hub.visited) + SEPARATOR + hub.url + SEPARATOR + hub.name);
	writeNumbered(config, "Recent hubs", "Hub ", lines, stored);
}

QuickDC::CompletedDownloads::CompletedDownloads(Preferences& config_)
	: config(config_)
{
	const char* group = "Completed";
	stored = total(config, group, LIMIT);

	for (int n = 0; n < stored; ++n)
	{
		const std::vector<std::string> parts = split(config.peekString(group, numbered("Entry ", n), ""), 4);
		CompletedDownload entry;
		if (parts.size() < 4 || !number(parts[0], entry.finished) || !number(parts[1], entry.size)
		    || parts[3].empty())
			continue;

		entry.tth = parts[2];
		entry.filename = parts[3];
		done.push_back(std::move(entry));
	}

	std::stable_sort(done.begin(), done.end(),
		[](const CompletedDownload& a, const CompletedDownload& b) { return a.finished > b.finished; });
}

bool QuickDC::CompletedDownloads::add(const CompletedDownload& entry)
{
	if (entry.filename.empty()) return false;

	/* Fetching a file twice is two entries; one completion reported twice is one. */
	for (const CompletedDownload& existing : done)
	{
		if (existing.filename != entry.filename) continue;
		const uint64_t apart = existing.finished > entry.finished ? existing.finished - entry.finished
		                                                          : entry.finished - existing.finished;
		if (apart <= REPEAT_WINDOW_SECONDS) return false;
	}

	done.push_back(entry);
	std::stable_sort(done.begin(), done.end(),
		[](const CompletedDownload& a, const CompletedDownload& b) { return a.finished > b.finished; });
	if (done.size() > static_cast<size_t>(LIMIT)) done.resize(LIMIT);
	store();
	return true;
}

bool QuickDC::CompletedDownloads::remove(const std::string& filename)
{
	const auto found = std::find_if(done.begin(), done.end(),
		[&filename](const CompletedDownload& one) { return one.filename == filename; });
	if (found == done.end()) return false;

	done.erase(found);
	store();
	return true;
}

bool QuickDC::CompletedDownloads::clear()
{
	if (done.empty()) return false;
	done.clear();
	store();
	return true;
}

void QuickDC::CompletedDownloads::store()
{
	std::vector<std::string> lines;
	for (const CompletedDownload& one : done)
		lines.push_back(std::to_string(one.finished) + SEPARATOR + std::to_string(one.size)
		                + SEPARATOR + one.tth + SEPARATOR + one.filename);
	writeNumbered(config, "Completed", "Entry ", lines, stored);
}
