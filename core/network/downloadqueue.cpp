/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/downloadqueue.h"


QuickDC::QueueManager::QueueManager() {
	QDBG("QueueManager::QueueManager()");
}

QuickDC::QueueManager::~QueueManager() {

}

void QuickDC::QueueManager::process() {

}

void QuickDC::QueueManager::add(QueueFile* qfile) {
	QDBG("Adding file to download queue: %s", qfile->filename);
}

void QuickDC::QueueManager::remove(QueueFile* qfile) {
	QDBG("Removing file from download queue: %s", qfile->filename);
}

size_t QuickDC::QueueManager::size() {
	return files.size();
}

void QuickDC::QueueManager::load() {
	QDBG("FIXME: load download queue");
}

void QuickDC::QueueManager::save() {
	QDBG("FIXME: save download queue");
}

