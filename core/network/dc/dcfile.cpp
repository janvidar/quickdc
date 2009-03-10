/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/user.h"
#include "network/dc/dcfile.h"

namespace QuickDC {

DCFile::DCFile(const std::string& file_) : user(0), size(-1) {
	file = file_;
}

DCFile::DCFile(User* user_, const std::string& file_) : user(user_), size(-1) {
	file = file_;
}

DCFile::DCFile(User* user_, const std::string& file_, uint64_t size_) : user(user_), size(size_) {
	file = file_;
}

DCFile::DCFile(const DCFile& copy) : user(copy.user), size(copy.size) {
	file = copy.file;
}

// NOTE: Do *NOT* delete user.
DCFile::~DCFile() {
}

void DCFile::setUser(User* user_) {
	user = user_;
}

User* DCFile::getUser() const {
	return user;
}

// Note: Compares MyList.DcLst and MyList.bz2 as the
//       same file for convenience.
// TODO: Will also compare hashes in the future.
//       The idea is that multiple files can act as a sources
//       for a single local file.
bool DCFile::operator==(const DCFile& dcf) {
	if (this == &dcf) return true;
// #warning "Not properly implemented, needs the hashing stuff." 
	return false;
}

}
