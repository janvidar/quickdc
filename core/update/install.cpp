/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "update/install.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

QuickDC::Update::Installation QuickDC::Update::currentInstallation()
{
	Installation here;
#if defined(__linux__)
	/* Set by the AppImage runtime to the file it was started from. */
	if (const char* image = std::getenv("APPIMAGE")) here.appImage = image;
#elif defined(__APPLE__)
	char path[4096];
	uint32_t size = sizeof(path);
	if (_NSGetExecutablePath(path, &size) == 0)
	{
		/* .../QuickDC.app/Contents/MacOS/QuickDC */
		std::error_code ec;
		const fs::path executable = fs::canonical(path, ec);
		const fs::path bundle = executable.parent_path().parent_path().parent_path();
		if (!ec && bundle.extension() == ".app") here.bundle = bundle.string();
	}
#elif defined(_WIN32)
	wchar_t path[MAX_PATH];
	const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if (length > 0 && length < MAX_PATH)
	{
		const fs::path executable(path);
		/* Only what the installer put there: it leaves its uninstaller beside the program. */
		std::error_code ec;
		if (fs::exists(executable.parent_path() / "unins000.exe", ec)) here.executable = executable.string();
	}
#endif
	return here;
}

#if defined(__linux__)

bool QuickDC::Update::install(const std::string& package, const Installation& here, std::string& why)
{
	if (here.appImage.empty())
	{
		why = "this is not running as an AppImage, so there is nothing to replace";
		return false;
	}

	/*
	 * Copied beside it and renamed over it, which is atomic and leaves the
	 * running process its own copy until it exits. A copy straight over it
	 * would change the file the process is reading its code from.
	 */
	const fs::path target(here.appImage);
	const fs::path staged = target.parent_path() / ("." + target.filename().string() + ".update");
	std::error_code ec;
	fs::copy_file(package, staged, fs::copy_options::overwrite_existing, ec);
	if (!ec) fs::permissions(staged, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
	                                 | fs::perms::others_read | fs::perms::others_exec, ec);
	if (!ec) fs::rename(staged, target, ec);
	if (ec)
	{
		std::error_code ignored;
		fs::remove(staged, ignored);
		why = "the AppImage at " + here.appImage + " could not be replaced: " + ec.message();
		return false;
	}

	fs::remove(package, ec);
	return true;
}

#elif defined(__APPLE__)

namespace {

/* A program run to its end, with its exit status as the answer. */
bool run(const char* const argv[])
{
	pid_t pid = 0;
	if (posix_spawn(&pid, argv[0], nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0)
		return false;
	int status = 0;
	if (waitpid(pid, &status, 0) != pid) return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}

bool QuickDC::Update::install(const std::string& package, const Installation& here, std::string& why)
{
	if (here.bundle.empty())
	{
		why = "this is not running from an application bundle, so there is nothing to replace";
		return false;
	}

	/*
	 * Unpacked beside the bundle, on the same volume, so that the swap is two
	 * renames: the old bundle aside, the new one into its place.
	 */
	const fs::path bundle(here.bundle);
	const fs::path parent = bundle.parent_path();
	const std::string tag = std::to_string(getpid());
	const fs::path staging = parent / (".QuickDC-update-" + tag);
	const fs::path aside = parent / (".QuickDC-old-" + tag + ".app");

	std::error_code ec;
	fs::remove_all(staging, ec);
	const std::string staging_path = staging.string();
	const char* const unpack[] = {"/usr/bin/ditto", "-x", "-k", package.c_str(), staging_path.c_str(), nullptr};
	if (!run(unpack))
	{
		fs::remove_all(staging, ec);
		why = "the update could not be unpacked beside " + here.bundle;
		return false;
	}

	const fs::path fresh = staging / bundle.filename();
	if (!fs::is_directory(fresh / "Contents" / "MacOS", ec))
	{
		fs::remove_all(staging, ec);
		why = "the update holds no " + bundle.filename().string();
		return false;
	}

	fs::rename(bundle, aside, ec);
	if (ec)
	{
		why = "the application at " + here.bundle + " could not be moved aside: " + ec.message();
		std::error_code ignored;
		fs::remove_all(staging, ignored);
		return false;
	}

	fs::rename(fresh, bundle, ec);
	if (ec)
	{
		/* Put back what was there, so a failed update leaves a working program. */
		std::error_code ignored;
		fs::rename(aside, bundle, ignored);
		fs::remove_all(staging, ignored);
		why = "the new application could not be moved into place: " + ec.message();
		return false;
	}

	std::error_code ignored;
	fs::remove_all(aside, ignored);
	fs::remove_all(staging, ignored);
	fs::remove(package, ignored);
	return true;
}

#elif defined(_WIN32)

bool QuickDC::Update::install(const std::string& package, const Installation& here, std::string& why)
{
	if (here.executable.empty())
	{
		why = "this was not installed by the installer, so there is nothing to replace";
		return false;
	}

	/*
	 * The same kind of installation as the one being replaced: one under
	 * Program Files was made for every user, and replacing it asks for the rights
	 * that takes; anything else was made for this user alone.
	 */
	const fs::path directory = fs::path(here.executable).parent_path();
	bool everyone = false;
	PWSTR programs = nullptr;
	if (SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programs) == S_OK)
	{
		const std::wstring root = fs::path(programs).lexically_normal().wstring();
		const std::wstring here_dir = directory.lexically_normal().wstring();
		everyone = _wcsnicmp(here_dir.c_str(), root.c_str(), root.size()) == 0;
	}
	CoTaskMemFree(programs);

	std::wstring command = L"\"" + fs::path(package).wstring() + L"\""
		L" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NORESTARTAPPLICATIONS"
		+ std::wstring(everyone ? L" /ALLUSERS" : L" /CURRENTUSER")
		+ L" /DIR=\"" + directory.wstring() + L"\"";

	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
	                    DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process))
	{
		why = "the installer could not be started (error " + std::to_string(GetLastError()) + ")";
		return false;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
}

#else

bool QuickDC::Update::install(const std::string&, const Installation&, std::string& why)
{
	why = "this platform has no packages to update from";
	return false;
}

#endif
