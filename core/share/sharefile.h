/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SHAREFILE_H
#define HAVE_QUICKDC_SHAREFILE_H

#include "quickdc.h"
#include <samurai/io/file.h>

#include "share/filetypes.h"
#include "hash/hashjob.h"
#include "share/searchrequest.h"

namespace QuickDC {
namespace Share {

class Dir;

/**
 * A file as perceived by others over the network.
 */
class File : public QuickDC::Hash::JobListener {
	public:
		File(const std::string& name, QuickDC::Share::Dir*);
		virtual ~File();

		/**
		 * Do extended checks on a potential search result to
		 * determine if it matches the search criterias.
		 * 
		 * - check if the size matches
		 * - check if the filetype matches
		 */
		inline bool match(uint64_t size, enum QuickDC::Share::SearchRequest::SizePolicy st) const;
		
		/**
		 * Indicate a search hit, and increase file popularity.
		 */
		inline void hit();
		
		/**
		 * Returns the public filename with rewritten/virtual root, 
		 * as perceived on network.
		 */
		const std::string& getPublicName();
		
		/**
		 * Returns the filename only, excluding the full path.
		 */
		std::string getBaseFileName() const { return file.getBaseName(); }
		
		/**
		 * Returns the full file name, including the full path.
		 */
		const char* getFileName() const { return file.getName().c_str(); }
		
		/**
		 * Returns a file handle to the file.
		 */
		const Samurai::IO::File& getFile() const { return file; }
		
		/**
		 * Returns the TTH of the file, or 0 if 
		 * the TTH isn't calculated yet.
		 */
		const char* getTTH() const { return tth; }
		
		/**
		 * Returns the file size.
		 */
		uint64_t getSize() const { return size; }
		
		int getLevel();
		
	protected:
		void EventFileStarted(const QuickDC::Hash::Job*);
		void EventFileQueued(const QuickDC::Hash::Job*);
		void EventFileHashed(const QuickDC::Hash::Job*);
		void EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t, uint64_t);
		
	protected:
		QuickDC::Share::Dir* directory;
		Samurai::IO::File file;
		std::string publicName;
		char* tth;
		uint64_t size;
		size_t ftype;
		size_t popularity;
};


inline bool QuickDC::Share::File::match(uint64_t ssize, enum QuickDC::Share::SearchRequest::SizePolicy st) const
{
	switch (st) {
		case Share::SearchRequest::SizeAny:   return true;
		case Share::SearchRequest::SizeExact: return (size == ssize);
		case Share::SearchRequest::SizeMin:   return (size >= ssize);
		case Share::SearchRequest::SizeMax:   return (size <= ssize);
	}
	return false;
}

inline void QuickDC::Share::File::hit()
{
	popularity++;
}

}
}

#endif // HAVE_QUICKDC_SHAREFILE_H
