/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "update/install.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

/* Replacing an AppImage is the one install that runs here; the others need a bundle or an installer. */
#ifdef __linux__
const bool INSTALLS_APPIMAGE = true;
#else
const bool INSTALLS_APPIMAGE = false;
#endif

#ifdef _WIN32
bool runningAsRoot() { return false; }
#else
bool runningAsRoot() { return ::geteuid() == 0; }
#endif

std::string slurp(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Scratch {
	fs::path root;
	Scratch()
	{
		std::error_code ec;
		root = fs::temp_directory_path(ec) / ("quickdc-install-" + std::to_string(::time(nullptr)) + "-"
		                                      + std::to_string(reinterpret_cast<uintptr_t>(this)));
		fs::create_directories(root, ec);
	}
	~Scratch()
	{
		std::error_code ec;
		fs::remove_all(root, ec);
	}
	void write(const fs::path& path, const std::string& text)
	{
		std::ofstream(path, std::ios::binary) << text;
	}
};

}

EXO_TEST(install_an_appimage_is_replaced_in_place,
{
	if (!INSTALLS_APPIMAGE) EXO_SKIP("an AppImage is replaced only on Linux");

	Scratch scratch;
	const fs::path running = scratch.root / "QuickDC.AppImage";
	const fs::path package = scratch.root / "updates-QuickDC-99.0.0.AppImage";
	scratch.write(running, "old");
	scratch.write(package, "new");

	QuickDC::Update::Installation here;
	here.appImage = running.string();
	std::string why;
	EXO_ASSERT(QuickDC::Update::install(package.string(), here, why));
	EXO_ASSERT(slurp(running) == "new");

	/* Runnable, and the package and anything staged gone. */
	const fs::perms mode = fs::status(running).permissions();
	EXO_ASSERT((mode & fs::perms::owner_exec) != fs::perms::none);
	EXO_ASSERT(!fs::exists(package));
	EXO_ASSERT(!fs::exists(scratch.root / ".QuickDC.AppImage.update"));
	return true;
});

EXO_TEST(install_without_an_installation_changes_nothing,
{
	Scratch scratch;
	const fs::path package = scratch.root / "package";
	scratch.write(package, "new");

	std::string why;
	EXO_ASSERT(!QuickDC::Update::install(package.string(), QuickDC::Update::Installation{}, why));
	EXO_ASSERT(!why.empty());
	return fs::exists(package);
});

EXO_TEST(install_into_a_directory_that_cannot_be_written_leaves_the_old_one,
{
	if (!INSTALLS_APPIMAGE) EXO_SKIP("an AppImage is replaced only on Linux");

	Scratch scratch;
	const fs::path directory = scratch.root / "locked";
	fs::create_directories(directory);
	const fs::path running = directory / "QuickDC.AppImage";
	const fs::path package = scratch.root / "package.AppImage";
	scratch.write(running, "old");
	scratch.write(package, "new");
	fs::permissions(directory, fs::perms::owner_read | fs::perms::owner_exec);

	QuickDC::Update::Installation here;
	here.appImage = running.string();
	std::string why;
	const bool installed = QuickDC::Update::install(package.string(), here, why);
	fs::permissions(directory, fs::perms::owner_all);

	/* Root writes wherever it likes, so there the refusal cannot be seen. */
	if (installed && runningAsRoot()) EXO_SKIP("running as root, which ignores the directory's mode");
	EXO_ASSERT(!installed);
	EXO_ASSERT(!why.empty());
	return slurp(running) == "old" && fs::exists(package);
});
