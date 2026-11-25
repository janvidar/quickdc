/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SHAREFOLDERS_H
#define HAVE_QUICKDC_SHAREFOLDERS_H

#include <string>
#include <vector>

namespace QuickDC {

class Preferences;

namespace Share {

/** A folder offered to other users: what they see it called, and where it is here. */
struct Folder {
	std::string name;
	std::string path;
};

/** Why a change to the folder list was not made. */
enum class FolderRefusal {
	None,
	/** That folder is shared already. */
	AlreadyShared,
	/** It is inside a folder that is shared, so every file in it already is. */
	InsideShared,
	/** There is no folder at that path to share. */
	NotADirectory,
	/** No shared folder has that path. */
	NotShared,
	/** Nothing would be left of the name to show. */
	NoName
};

/*
 * The list as it is stored, in [Shares]:
 *
 *     Total shares=1
 *     Share 0=Music:/home/user/Music
 *
 * The first colon splits the pair, since a path may hold more of them and a name
 * may not. An entry with nothing on either side of it is not a folder.
 */
std::vector<Folder> readFolders(const Preferences& config);

/** Write 'folders' over the list, leaving none of a longer one behind. */
void writeFolders(Preferences& config, const std::vector<Folder>& folders);

/** 'path' as one spelling: separators collapsed, "." and ".." resolved, none trailing. */
std::string cleanFolderPath(const std::string& path);

/** 'name' without the separator, which it cannot hold, and without surrounding space. */
std::string cleanFolderName(const std::string& name);

/** The name a folder is shown by when nobody chose one: its own, or "share". */
std::string defaultFolderName(const std::string& path);

/** Whether 'path' (clean) is a shared folder or inside one; None where it is neither. */
FolderRefusal coveredBy(const std::vector<Folder>& folders, const std::string& path);

}
}

#endif // HAVE_QUICKDC_SHAREFOLDERS_H
