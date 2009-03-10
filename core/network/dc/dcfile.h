/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCFILE_H
#define HAVE_QUICKDC_DCFILE_H

#include "quickdc.h"
#include <string>

namespace QuickDC {

class User;

class DCFile {

	public:
		DCFile(const std::string& file);
		DCFile(User* user, const std::string& file);
		DCFile(User* user, const std::string&, uint64_t size);
		DCFile(const DCFile& copy);
		virtual ~DCFile();

		void setUser(User* user);
		User* getUser() const;

		// Note: Compares MyList.DcLst and MyList.bz2 as the
		//       same file for convenience.
		// TODO: Will also compare hashes in the future.
		//       The idea is that multiple files can act as a sources
		//       for a single local file.
		bool operator==(const DCFile& dcf);

	protected:
		std::string file;
		User* user;
		int64_t size;
		
		// file type, determined from the file extension.
		int ftype;
};

}

#endif // HAVE_QUICKDC_DCFILE_H
