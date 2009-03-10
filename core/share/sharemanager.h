/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SHAREMANAGER_H
#define HAVE_QUICKDC_SHAREMANAGER_H

#include <vector>
#include <map>
#include <string>

#include "quickdc.h"
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

class Manager {

	public:
		Manager(QuickDC::Preferences* config, QuickDC::Hash::Manager* mgr);
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
		
	protected:	
		std::vector<QuickDC::Share::Dir*> dirs;
		std::vector< std::pair<const char*, QuickDC::Share::File*> > searchTable;
		std::map<std::string, QuickDC::Share::File*> tthTable;

		// The number of files and bytes shared.
		uint64_t size;
		uint32_t files;
		QuickDC::Preferences* config;
		QuickDC::Hash::Manager* hash;
		bool filelists;

	friend class QuickDC::Share::Dir;
	friend class QuickDC::Share::File;
};

}
}

#endif // HAVE_QUICKDC_SHAREMANAGER_H
