/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "share/sharefolders.h"
#include "config/preferences.h"
#include "config/settings.h"

#include <algorithm>
#include <filesystem>

namespace {

const char GROUP[] = "Shares";
const char SEPARATOR = ':';

/* More than anyone shares, and a bound on a corrupt total; Share::Manager's too. */
const int LIMIT = 1024;

std::string shareKey(int index)
{
	return "Share " + std::to_string(index);
}

std::string trimmed(const std::string& text)
{
	const size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	const size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

}

std::vector<QuickDC::Share::Folder> QuickDC::Share::readFolders(const QuickDC::Preferences& config)
{
	std::vector<Folder> folders;

	const int total = std::clamp(QuickDC::Settings::read(config, QuickDC::Settings::TotalShares), 0, LIMIT);

	for (int n = 0; n < total; ++n)
	{
		const std::string line = config.peekString(GROUP, shareKey(n), "");
		const size_t split = line.find(SEPARATOR);
		if (split == std::string::npos || split == 0 || split + 1 == line.size()) continue;

		folders.push_back(Folder{line.substr(0, split), line.substr(split + 1)});
	}

	return folders;
}

void QuickDC::Share::writeFolders(QuickDC::Preferences& config, const std::vector<Folder>& folders)
{
	const int count = static_cast<int>(folders.size());

	for (int n = 0; n < count; ++n)
		config.setString(GROUP, shareKey(n), folders[n].name + SEPARATOR + folders[n].path);

	/* Every numbered key past the end, however it got there. */
	for (const std::string& key : config.keysIn(GROUP))
	{
		if (key.rfind("Share ", 0) != 0) continue;

		const std::string number = key.substr(6);
		if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos) continue;
		if (number.size() > 6 || std::stoi(number) >= count) config.remove(GROUP, key);
	}

	config.setNumber(GROUP, QuickDC::Settings::TotalShares.key, count);
}

std::string QuickDC::Share::cleanFolderPath(const std::string& path)
{
	const std::string given = trimmed(path);
	if (given.empty()) return given;

	/* Forward slashes on Windows too, which is how every path here is compared. */
	std::string clean = std::filesystem::path(given).lexically_normal().generic_string();

	while (clean.size() > 1 && clean.back() == '/' && !(clean.size() == 3 && clean[1] == ':'))
		clean.pop_back();
	return clean;
}

std::string QuickDC::Share::cleanFolderName(const std::string& name)
{
	std::string clean = name;
	clean.erase(std::remove(clean.begin(), clean.end(), SEPARATOR), clean.end());
	return trimmed(clean);
}

std::string QuickDC::Share::defaultFolderName(const std::string& path)
{
	const std::string own = cleanFolderName(std::filesystem::path(cleanFolderPath(path)).filename().string());
	return own.empty() ? std::string("share") : own;
}

QuickDC::Share::FolderRefusal QuickDC::Share::coveredBy(const std::vector<Folder>& folders,
                                                        const std::string& path)
{
	for (const Folder& folder : folders)
	{
		const std::string have = cleanFolderPath(folder.path);

		if (have == path) return FolderRefusal::AlreadyShared;

		const std::string prefix = have == "/" ? have : have + "/";
		if (path.compare(0, prefix.size(), prefix) == 0) return FolderRefusal::InsideShared;
	}

	return FolderRefusal::None;
}
