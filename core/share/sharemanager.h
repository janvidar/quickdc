/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SHAREMANAGER_H
#define HAVE_QUICKDC_SHAREMANAGER_H

#include <vector>
#include <map>
#include <memory>
#include <string>

#include "quickdc.h"
#include "hash/hashjob.h"
#include "network/dc/dcfile.h"

namespace Samurai {
	namespace IO {
		class File;
		class Buffer;
	}
}

namespace QuickDC {
namespace Hash {
class Manager;
}
class Preferences;

}

namespace QuickDC {
namespace Share {

class SearchRequest;
class File;
class Dir;
class AttachmentListener;

class Manager : public QuickDC::Hash::JobListener {

	public:
		/**
		 * @param dir where the generated file lists are written, so they can
		 *        be pointed somewhere other than the real ones.
		 */
		Manager(QuickDC::Preferences* config, QuickDC::Hash::Manager* mgr,
		        const char* dir = "~/.quickdc");
		virtual ~Manager();

		/**
		 * Read configuration and initialize.
		 */
		void initialize();
		
		/**
		 * Returns the number bytes being shared at this moment.
		 * Please note that this number is updated only after 
		 * recalculations.
		 */
		uint64_t getSharedSize() { return size; }

		/**
		 * Returns the number of files being shared.
		 */
		uint32_t getFiles() { return files; }

		/**
		 * Rebuild share database and update file indexes.
		 * If nothing has changed since the last time this 
		 * was called, this operation is fairly cheap.
		 * 
		 * NOTE: This will write a file to disk where
		 * all hashes are stored so that rebuilding between 
		 * different sessions is fairly cheap as well.
		 */
		void rebuild();
		void createFileLists(bool incomplete);
		
		/**
		 * Search the local file database.
		 */
		void search(SearchRequest* req);

		/**
		 * Get a file from the share database based on the public
		 * name it has on DC (as perceived by other clients).
		 */
		Samurai::IO::File* getFile(const char* dcname) const;
		
		/**
		 * Get a file from the share database based on the TTH
		 * sum.
		 */
		Samurai::IO::File* getFileByTTH(const char* sum);

		/**
		 * Offer a file to other clients by its tiger tree hash alone, without
		 * sharing it. This is what a chat attachment needs: the message carries
		 * a magnet URI naming the hash, and whoever has that URI can fetch the
		 * file.
		 *
		 * The file stays invisible everywhere else. It is in no file list, no
		 * search by name matches it, and the number of files and bytes a hub is
		 * told about does not change. The hash is therefore the capability:
		 * anyone who has it can fetch the file, and there is no other way to
		 * discover that we hold it.
		 *
		 * Hashing is asynchronous, so this returns long before the hash of a
		 * large file is known. The listener is called on the thread that drives
		 * the core, from Hash::Manager::process() - unless the file has been
		 * hashed before, in which case the hash comes out of the hash cache and
		 * is reported before this returns.
		 *
		 * An attachment is remembered in the configuration as soon as its hash is
		 * known, under [Attachments], so a magnet URI handed out in a message
		 * still works after a restart. A remembered file that has been edited
		 * since - one the hash cache can no longer vouch for - is dropped when
		 * the configuration is read, since the hash no longer describes it.
		 *
		 * @param path a regular, readable file.
		 * @param listener told the hash once it is known, or why it never will
		 *        be. Attaching a file that is already attached, or one whose
		 *        hash is known because it is genuinely shared, reports the hash
		 *        it already has.
		 * @return false when there is nothing to work with - no path, no
		 *         listener, or a path that is not a readable regular file - and
		 *         then the listener is not called at all. Anything that can only
		 *         be discovered later is reported to the listener instead.
		 */
		bool attach(const char* path, QuickDC::Share::AttachmentListener* listener);

	protected:
		/**
		 * Hash::JobListener, for the files handed to attach(). A shared file
		 * hashes itself; these have no Share::File to do it for them until the
		 * hash is known.
		 */
		void EventFileStarted(const QuickDC::Hash::Job*) override;
		void EventFileQueued(const QuickDC::Hash::Job*) override;
		void EventFileHashed(const QuickDC::Hash::Job*) override;
		void EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t, uint64_t) override;

	protected:
		/**
		 * Add a directory to the share manager.
		 * This is a very fast operation since the 
		 * share database is not being rebuild at this point.
		 * To rebuild the database, use rebuild().
		 *
		 * The ShareManager will ensure that the file/share
		 * is not added more than once.
		 */
		void addShare(QuickDC::Share::Dir* dir);

		/**
		 * Remove a directory from the share manager.
		 * This is a very fast operation since the share
		 * database aren't performed at this point.
		 * To rebuild the database, use rebuild().
		 */
		void removeShare(QuickDC::Share::Dir* dir);

		void addSize(uint64_t sz);

		
		/**
		 * Create file lists:
		 * - files.xml
		 * - files.xml.bz2
		 */
		void createFileList(Samurai::IO::Buffer* xmlout, QuickDC::Share::Dir* dir = 0, bool incomplete = false);
		
		/**
		 * This ensures file can be looked up by TTH.
		 */
		void addLookupFile(const char* file, const char* tth, QuickDC::Share::File*);

		/**
		 * A file handed to attach(): offered by TTH, part of no share.
		 */
		struct Attachment {
			/** The path as attach() was given it, which is what the listener is told. */
			std::string path;
			/** The same file resolved, which is how the hash manager names it. */
			std::string name;
			/** Empty until the file has been hashed. */
			std::string tth;
			uint64_t size = 0;
			/** Owns 'file', and is deliberately not one of 'dirs'. */
			QuickDC::Share::Dir* dir = 0;
			/** Null while the hash is unknown, and when a shared file has the same hash. */
			QuickDC::Share::File* file = 0;
			/** Waiting to be told the hash. Emptied once they have been. */
			std::vector<QuickDC::Share::AttachmentListener*> listeners;

			~Attachment();
		};

		/**
		 * The file behind a TTH, whether it is shared or attached.
		 */
		QuickDC::Share::File* findByTTH(const char* tth) const;

		QuickDC::Share::Manager::Attachment* findAttachment(const std::string& name) const;
		bool isAttachment(const QuickDC::Share::File* ptr) const;

		/**
		 * Make an attachment reachable by its hash, and by nothing else.
		 *
		 * @return false if it cannot be, which the caller has to report.
		 */
		bool registerAttachment(QuickDC::Share::Manager::Attachment& record);

		/**
		 * Tell an attachment's listeners it cannot be offered, and forget it.
		 */
		void dropAttachment(QuickDC::Share::Manager::Attachment& record, const char* reason);

		/**
		 * Report the attachments whose hash is never coming.
		 */
		void sweepAttachments();

		/**
		 * Write the attachments to the configuration, so a magnet URI handed out
		 * in a message still works after a restart.
		 *
		 * The whole group is written every time, since Preferences cannot
		 * enumerate the keys in a group and a Total left behind from a longer
		 * list would name entries that are no longer there.
		 */
		void saveAttachments();

		/**
		 * Offer the remembered attachments again.
		 */
		void loadAttachments();

		/**
		 * Offer a remembered file again, if it is still the file the remembered
		 * hash was taken from.
		 *
		 * @return false when it is not, and the entry should be forgotten.
		 */
		bool adoptAttachment(const std::string& tth, const std::string& path);

	protected:
		std::vector<QuickDC::Share::Dir*> dirs;
		/* Owns the name; the callers pass one that does not outlive the call. */
		std::vector< std::pair<std::string, QuickDC::Share::File*> > searchTable;
		std::map<std::string, QuickDC::Share::File*> tthTable;
		/* Held by pointer so a record keeps its address while a listener that is
		   being called from it attaches another file. */
		std::vector< std::unique_ptr<QuickDC::Share::Manager::Attachment> > attachments;

		// The number of files and bytes shared.
		uint64_t size;
		uint32_t files;
		QuickDC::Preferences* config;
		QuickDC::Hash::Manager* hash;
		std::string listdir;
		bool filelists;
		/* Set while an attachment's Share::File is being built, since such a file
		   reaches the manager the same way a shared one does. */
		bool attaching;

	friend class QuickDC::Share::Dir;
	friend class QuickDC::Share::File;
};

}
}

#endif // HAVE_QUICKDC_SHAREMANAGER_H
