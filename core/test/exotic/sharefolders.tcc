/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The shared folders: how the list is stored, which folders may join it, and the
 * share following a change at once rather than at the next start.
 */

#include "quickdc.h"
#include "api/core.h"
#include "config/preferences.h"
#include "hash/hashmanager.h"
#include "share/sharefolders.h"
#include "share/sharemanager.h"
#include "share/sharefile.h"
#include "share/searchrequest.h"

#include <thread>
#include <samurai/io/file.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdio.h>
#include <string>
#include <samurai/posix.h>
#include <vector>

using QuickDC::Share::Folder;
using QuickDC::Share::FolderRefusal;

namespace {

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_sharefolders_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

bool writeFile(const std::string& path, const std::string& content)
{
	FILE* fh = fopen(path.c_str(), "wb");
	if (!fh) return false;
	const bool ok = fwrite(content.data(), 1, content.size(), fh) == content.size();
	fclose(fh);
	return ok;
}

class Replies : public QuickDC::Share::SearchReplyHandler {
	public:
		void EventSearchReply(QuickDC::Share::SearchReply* reply) override
		{
			names.push_back(reply->getFile()->getPublicName());
		}

		std::vector<std::string> names;
};

/* A core with a share manager over scratch folders, and a hash manager to drive by hand. */
class Shares {
	public:
		Shares()
			: root(scratch("root"))
			, cfg(scratch("config.conf"))
			, ses(scratch("session.conf"))
			, cache(scratch("hash.cache"))
			, lists(scratch("lists"))
		{
			std::filesystem::create_directories(root);
			std::filesystem::create_directories(lists);
			core = std::make_unique<QuickDC::Core>(cfg, ses);
			hashes = std::make_unique<QuickDC::Hash::Manager>(cache.c_str());
			manager = new QuickDC::Share::Manager(core->config, hashes.get(), lists.c_str());
			delete core->shares;
			core->shares = manager;
		}

		~Shares()
		{
			core->shares = 0;
			delete manager;
			core.reset();
			hashes.reset();
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
			std::filesystem::remove_all(lists, ignored);
			Samurai::IO::File(cfg).remove();
			Samurai::IO::File(ses).remove();
			Samurai::IO::File(cache).remove();
		}

		Shares(const Shares&) = delete;
		Shares& operator=(const Shares&) = delete;

		std::string folder(const std::string& name)
		{
			const std::string path = root + "/" + name;
			std::filesystem::create_directories(path);
			return path;
		}

		void file(const std::string& path, const std::string& content)
		{
			writeFile(path, content);
		}

		bool hashed()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			while (hashes->process() || hashes->isBusy())
			{
				if (std::chrono::steady_clock::now() > deadline) return false;
				std::this_thread::sleep_for(std::chrono::microseconds(1000));
			}
			hashes->process();
			return true;
		}

		std::vector<std::string> search(const char* word)
		{
			Replies replies;
			QuickDC::Share::SearchRequest request(&replies);
			request.addInclusion(word);
			manager->search(&request);
			return replies.names;
		}

		std::string root;
		std::string cfg;
		std::string ses;
		std::string cache;
		std::string lists;
		std::unique_ptr<QuickDC::Core> core;
		std::unique_ptr<QuickDC::Hash::Manager> hashes;
		QuickDC::Share::Manager* manager = 0;
};

}

/* The list is read in the shape it has always been stored in. */
EXO_TEST(sharefolders_the_stored_list_is_read_as_it_was_written,
{
	const std::string path = scratch("read.conf");
	EXO_ASSERT(writeFile(path,
		"[Shares]\n"
		"Total shares=4\n"
		"Share 0=Music:/home/me/Music\n"
		"Share 1=\n"
		"Share 2=C:/Windows:path\n"
		"Share 3=:/nameless\n"));

	{
		QuickDC::Preferences prefs(path);
		const std::vector<Folder> folders = QuickDC::Share::readFolders(prefs);

		EXO_ASSERT_EQ_UINT(folders.size(), 2u);
		EXO_ASSERT(folders[0].name == "Music" && folders[0].path == "/home/me/Music");
		EXO_ASSERT(folders[1].name == "C" && folders[1].path == "/Windows:path");
	}

	Samurai::IO::File(path).remove();
	return 1;
});

/* A shorter list leaves none of the longer one behind. */
EXO_TEST(sharefolders_writing_a_shorter_list_leaves_no_tail,
{
	const std::string path = scratch("write.conf");
	EXO_ASSERT(writeFile(path,
		"[Shares]\nTotal shares=3\nShare 0=a:/a\nShare 1=b:/b\nShare 2=c:/c\nShare 7=x:/x\n"));

	{
		QuickDC::Preferences prefs(path);
		QuickDC::Share::writeFolders(prefs, {Folder{"b", "/b"}});

		EXO_ASSERT(prefs.peekNumber("Shares", "Total shares", 0) == 1);
		EXO_ASSERT(prefs.peekString("Shares", "Share 0", "") == "b:/b");
		EXO_ASSERT(!prefs.haveSetting("Shares", "Share 1"));
		EXO_ASSERT(!prefs.haveSetting("Shares", "Share 2"));
		EXO_ASSERT(!prefs.haveSetting("Shares", "Share 7"));
	}

	Samurai::IO::File(path).remove();
	return 1;
});

EXO_TEST(sharefolders_a_path_has_one_spelling,
{
	/* Compared with what resolvePath() makes of the clean spelling, which has a drive on Windows. */
	const std::string music = Samurai::IO::File::resolvePath("/home/me/Music");
	EXO_ASSERT(QuickDC::Share::cleanFolderPath(" /home//me/./Music/ ") == music);
	EXO_ASSERT(QuickDC::Share::cleanFolderPath("/home/me/x/../Music") == music);
	EXO_ASSERT(QuickDC::Share::cleanFolderPath("/") == Samurai::IO::File::resolvePath("/"));
	EXO_ASSERT(QuickDC::Share::cleanFolderPath("  ").empty());
	return 1;
});

EXO_TEST(sharefolders_a_name_is_what_a_folder_is_called_unless_chosen,
{
	EXO_ASSERT(QuickDC::Share::defaultFolderName("/home/me/Music/") == "Music");
	EXO_ASSERT(QuickDC::Share::defaultFolderName("/") == "share");
	EXO_ASSERT(QuickDC::Share::cleanFolderName(" a:b: ") == "ab");
	return 1;
});

/* A folder already shared, or inside one, is shared already. A parent is not. */
EXO_TEST(sharefolders_what_is_shared_already_is_not_shared_again,
{
	const std::vector<Folder> folders = {Folder{"m", "/home/me/Music"}};

	EXO_ASSERT(QuickDC::Share::coveredBy(folders, "/home/me/Music") == FolderRefusal::AlreadyShared);
	EXO_ASSERT(QuickDC::Share::coveredBy(folders, "/home/me/Music/Jazz") == FolderRefusal::InsideShared);
	EXO_ASSERT(QuickDC::Share::coveredBy(folders, "/home/me/Musical") == FolderRefusal::None);
	EXO_ASSERT(QuickDC::Share::coveredBy(folders, "/home/me") == FolderRefusal::None);
	EXO_ASSERT(QuickDC::Share::coveredBy({Folder{"r", "/"}}, "/anything") == FolderRefusal::InsideShared);
	return 1;
});

/* A folder added is searched at once, and one taken out is not. */
EXO_TEST(sharefolders_a_folder_is_shared_and_unshared_without_a_restart,
{
	Shares s;
	const std::string music = s.folder("Music");
	s.file(music + "/tune.ogg", "a tune");

	EXO_ASSERT(s.manager->addFolder(music, "", nullptr) == FolderRefusal::None);
	EXO_ASSERT(s.hashed());

	EXO_ASSERT_EQ_UINT(s.manager->getFolders().size(), 1u);
	EXO_ASSERT(s.manager->getFolders()[0].name == "Music");
	EXO_ASSERT(s.core->config->peekString("Shares", "Share 0", "") == "Music:" + music);
	EXO_ASSERT_EQ_UINT(s.manager->getFiles(), 1u);

	const std::vector<std::string> found = s.search("tune");
	EXO_ASSERT_EQ_UINT(found.size(), 1u);
	if (!found.empty()) EXO_ASSERT(found[0] == "/Music/tune.ogg");

	EXO_ASSERT(s.manager->removeFolder(music + "/", nullptr) == FolderRefusal::None);
	EXO_ASSERT(s.search("tune").empty());
	EXO_ASSERT_EQ_UINT(s.manager->getFiles(), 0u);
	EXO_ASSERT_EQ_UINT(s.manager->getSharedSize(), 0u);
	EXO_ASSERT(s.core->config->peekNumber("Shares", "Total shares", -1) == 0);
	EXO_ASSERT(!s.core->config->haveSetting("Shares", "Share 0"));
	return 1;
});

EXO_TEST(sharefolders_a_change_that_cannot_be_made_says_why,
{
	Shares s;
	const std::string music = s.folder("Music");
	const std::string jazz = s.folder("Music/Jazz");

	EXO_ASSERT(s.manager->addFolder(music, "tunes", nullptr) == FolderRefusal::None);
	EXO_ASSERT(s.manager->addFolder(music, "again", nullptr) == FolderRefusal::AlreadyShared);
	EXO_ASSERT(s.manager->addFolder(jazz, "", nullptr) == FolderRefusal::InsideShared);
	EXO_ASSERT(s.manager->addFolder(s.root + "/absent", "", nullptr) == FolderRefusal::NotADirectory);
	EXO_ASSERT(s.manager->removeFolder(jazz, nullptr) == FolderRefusal::NotShared);
	EXO_ASSERT(s.manager->renameFolder(music, " : ", nullptr) == FolderRefusal::NoName);
	EXO_ASSERT_EQ_UINT(s.manager->getFolders().size(), 1u);
	return 1;
});

/* A rename is what others see the files under; the hashes are not taken again. */
EXO_TEST(sharefolders_a_renamed_folder_is_found_under_its_new_name,
{
	Shares s;
	const std::string music = s.folder("Music");
	const std::string video = s.folder("Video");
	s.file(music + "/tune.ogg", "a tune");
	s.file(video + "/film.mkv", "a film");

	EXO_ASSERT(s.manager->addFolder(music, "", nullptr) == FolderRefusal::None);
	EXO_ASSERT(s.manager->addFolder(video, "", nullptr) == FolderRefusal::None);
	EXO_ASSERT(s.hashed());

	EXO_ASSERT(s.manager->renameFolder(music, "Songs", nullptr) == FolderRefusal::None);
	EXO_ASSERT(!s.hashes->isBusy());

	const std::vector<std::string> found = s.search("tune");
	EXO_ASSERT_EQ_UINT(found.size(), 1u);
	if (!found.empty()) EXO_ASSERT(found[0] == "/Songs/tune.ogg");

	/* In its place in the list, which is the order the file list is in. */
	EXO_ASSERT(s.manager->getFolders()[0].name == "Songs");
	EXO_ASSERT(s.core->config->peekString("Shares", "Share 0", "") == "Songs:" + music);
	EXO_ASSERT(s.core->config->peekString("Shares", "Share 1", "") == "Video:" + video);
	EXO_ASSERT_EQ_UINT(s.manager->getFiles(), 2u);
	return 1;
});

/* Content in two folders is still found by hash once one of them goes. */
EXO_TEST(sharefolders_a_hash_held_twice_survives_one_folder_going,
{
	Shares s;
	const std::string one = s.folder("One");
	const std::string two = s.folder("Two");
	s.file(one + "/same.bin", "the same bytes");
	s.file(two + "/same.bin", "the same bytes");

	EXO_ASSERT(s.manager->addFolder(one, "", nullptr) == FolderRefusal::None);
	EXO_ASSERT(s.manager->addFolder(two, "", nullptr) == FolderRefusal::None);
	EXO_ASSERT(s.hashed());

	EXO_ASSERT_EQ_UINT(s.search("same").size(), 2u);

	struct First : QuickDC::Share::SearchReplyHandler {
		QuickDC::Share::File* file = nullptr;
		void EventSearchReply(QuickDC::Share::SearchReply* reply) override
		{
			if (!file) file = reply->getFile();
		}
	} first;
	QuickDC::Share::SearchRequest request(&first);
	request.addInclusion("same");
	s.manager->search(&request);

	QuickDC::Share::File* shared = first.file;
	std::string tth;
	EXO_ASSERT_NOT_NULL(shared);
	if (shared) tth = shared->getTTH() ? shared->getTTH() : "";
	EXO_ASSERT(!tth.empty());

	Samurai::IO::File* before = s.manager->getFileByTTH(tth.c_str());
	EXO_ASSERT_NOT_NULL(before);

	const std::string holder = before && before->getName().find("/One/") != std::string::npos ? one : two;
	EXO_ASSERT(s.manager->removeFolder(holder, nullptr) == FolderRefusal::None);

	Samurai::IO::File* after = s.manager->getFileByTTH(tth.c_str());
	EXO_ASSERT_NOT_NULL(after);
	if (after) EXO_ASSERT(after->getName().find(holder + "/") == std::string::npos);
	return 1;
});
