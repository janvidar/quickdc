/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SHAREDIR_H
#define HAVE_QUICKDC_SHAREDIR_H

#include <vector>
#include "quickdc.h"

namespace QuickDC {
namespace Share {

class File;
class Manager;

/**
 * This class represents a shared directory.
 * Such a directory contains files (DCShareFile) and subdirectories DCShareDir.
 * The topmost directory does not have a parent, but may have a "fake" root, 
 * which appears to others as if it is the root directory.
 * 
 * For example: the shared directory "/home/user/shares/" with a fakeroot
 * called "files", will be perceived as "/files/" to other users.
 *
 * If a DCSharedDir is deleted, all "children" are deleted automatically.
 */
class Dir
{
	
	public:
		/**
		 * Creates a top directory. This does not have a parent.
		 */
		Dir(const std::string& path, const std::string& name, QuickDC::Share::Manager* manager);

		/**
		 * Creates a shared subdirectory.
		 */
		Dir(const std::string& name, QuickDC::Share::Dir* parent);

		virtual ~Dir();

		const std::string& getBaseName() const { return basename; }
		const std::string& getName() const { return name; }
		const std::string& getPath() const { return path; }

		uint64_t getSize() const { return size; }
		int getLevel();
		
		QuickDC::Share::Dir*  getDir(const char* name);
		QuickDC::Share::File* getFile(const char* name);
		
	protected:
		void scan();

		void addSize(uint64_t size);
	
		QuickDC::Share::Dir* parent;
		QuickDC::Share::Manager* manager;
		std::vector<QuickDC::Share::Dir*> subdirs;
		std::vector<QuickDC::Share::File*> files;

		std::string path;
		std::string name;
		std::string basename;

		uint64_t size;
		size_t filecount;
		
	friend class QuickDC::Share::File;
	friend class QuickDC::Share::Manager;

};

}
}

#endif // HAVE_QUICKDC_SHAREDIR_H
