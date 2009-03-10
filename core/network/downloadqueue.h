/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DOWNLOADQUEUE_H
#define HAVE_QUICKDC_DOWNLOADQUEUE_H

#include "quickdc.h"
#include <string.h>
#include <stdlib.h>
#include <samurai/timestamp.h>
#include <vector>

namespace QuickDC {

class FileSource;
class Timer;

/**
 * Format:
 * file: {char*}       (required)
 * time:               (required)
 * size: {size}        (optional)
 * tth:  {char*}       (optional)
 * source: {char*}     (optional)
 * chunks: {int-int,}  (optional)
 *
 * Example of a queued file encoded on disk.
 *
 * file: /home/user/downloads/file.bin (1)
 * time: 1138411637 (2)
 * size: 2097152 (3)
 * tth: MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A (4)
 * source: dchub://myhub:411/{user name in base32}/file.bin (5)
 * source: adc://adchub:7817/ACSTOWQXZASA/TTH:MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A
 * source: adc://adchub:7817/Q2AH7321WAS1/TTH:MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A
 * chunks: 0-262144,524288-1048576 (6)
 *
 * This means; we have a file locally called file.bin (1), which was started downloaded
 * time Saturday January 28, 2006 (2), encoded in seconds since the UNIX epoch.
 * The file size is exactly 2MB (3), and we have a root hash of it (4).
 * We have 3 different sources (5) for the file. 
 * Currently the first 256KBytes are downloaded, followed by a 256KBytes gap,
 * the next 512Kbytes are completed, and the last 1024 kbytes are not (6)
 */
class QueueFile {
	public:
		enum Priority {
			PrioPaused,
			PrioLow,
			PrioNormal,
			PrioHigh
		};
	
		QueueFile(char* filename, char* tth);
		virtual ~QueueFile();
		
		void addSource(FileSource* source);
		void removeSource(FileSource* source);
		void expireSources();
		
		void countSources();
		void countActiveSources();
	
	protected:
		char* filename;                    // local filename
		char* tth;                         // tth
		int64_t size;                      // size of file (-1 if unknown)
		Samurai::TimeStamp added;           // the time the file was queued
		std::vector<FileSource*> sources;
		enum Priority priority;
		
		friend class QueueManager;
};

class QueueManager {
	public:
		QueueManager();
		virtual ~QueueManager();
		
		/**
		 * Do timely
		 */
		void process();
		
		void add(QueueFile*);
		void remove(QueueFile*);
		
		size_t size();
		
		/**
		 *
		 */
		QueueFile* getFileByUser(const char* userID);
		
	protected:
		void load();
		void save();
		
		
	protected:
		std::vector<QueueFile*> files;
		QuickDC::Timer* timer;
};


}

#endif // HAVE_QUICKDC_DOWNLOADQUEUE_H
