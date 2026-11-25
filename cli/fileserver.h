/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_FILESERVER_H
#define HAVE_QUICKDC_CLI_FILESERVER_H

#include <atomic>
#include <string>
#include <thread>

namespace QuickDC::Cli {

/**
 * The files of one directory over plain HTTP on the loopback address, so a
 * script can hand a core something to fetch without the internet: GET only, one
 * connection at a time, each closed after its answer.
 */
class FileServer {
	public:
		FileServer() = default;
		~FileServer();

		FileServer(const FileServer&) = delete;
		FileServer& operator=(const FileServer&) = delete;

		/** Listening by the time this returns; false, with 'why', where it cannot. */
		bool start(int port, std::string root, std::string& why);

		/** How many requests were answered with a file. */
		int served() const { return files; }

	private:
		void run();
		void answer(int client);

		std::string root;
		int listener = -1;
		int wake[2] = {-1, -1};
		std::thread thread;
		std::atomic<int> files{0};
};

}

#endif // HAVE_QUICKDC_CLI_FILESERVER_H
