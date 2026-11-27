/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_UPDATE_INSTALL_H
#define HAVE_QUICKDC_UPDATE_INSTALL_H

#include <string>

namespace QuickDC {
namespace Update {

/**
 * Where this program is installed, so that it can be replaced. What is set
 * depends on the platform, and an empty one means "not installed in a way an
 * update can replace", which is a build run from its build tree.
 */
struct Installation {
	/** Linux: the AppImage that is running ($APPIMAGE). */
	std::string appImage;
	/** macOS: the .app bundle that is running. */
	std::string bundle;
	/** Windows: the executable that is running, in the directory the installer chose. */
	std::string executable;
};

/** This program's installation, as the platform describes it. */
Installation currentInstallation();

/**
 * Put 'package' (an UpdateReady's path) in place of 'here', as the program exits:
 * done before this returns on Linux and macOS, and on Windows by an installer
 * started detached, which waits for this process to go.
 *
 * @return false with 'why' saying so, leaving the installation as it was.
 */
bool install(const std::string& package, const Installation& here, std::string& why);

}
}

#endif // HAVE_QUICKDC_UPDATE_INSTALL_H
