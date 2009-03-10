/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_FILESOURCE_H
#define HAVE_QUICKDC_FILESOURCE_H

#include "quickdc.h"
#include <string.h>
#include <stdlib.h>
#include <samurai/timestamp.h>
#include <vector>

namespace QuickDC {

/**
 * A file source is basically a source for downloading a specific file.
 * This does not contain any information about the file itself.
 * For obvious reasons this scheme works best with the ADC protocol, but 
 * the NMDC protocol with extensions support alot of the needed stuff here, 
 * except the needed clientID and thus the base32 encoded nick is used instead.
 *
 * This is basically a URL notation.
 *
 * Examples:
 * dchub://host:port/clientID/file (1)
 *   adc://host:port/clientID/TTH:xxxxxxxxxxx
 *
 * A source for a directory is encoded as:
 * {protocol}://host:port/{clientID}/dir/
 *
 * A source for the root directory (all shares) is encoded as follows:
 * {protocol}://host:port/{clientID}/ (2)
 *
 * A source for the file list (index) is encoded as follows:
 * {protocol}://host:port/{clientID} (3)
 *
 * 1) NOTE: clientID is basically a nick for 'dchub' protocol,
 *          This will have to be escaped so that it does not contain 
 *          any characters that will confuse decoding the URL.
 *          that means '/'.
 * 2) NOTE: Must have trailing slash. See 4.
 * 3) Without the trailing slash this means download the file listing,
 *    however with the trailing slash this means download the root
 *    directory.
 */
class FileSource {
	public:

		/**
		 * A filesource might be a on the form: adc://{server}:{port}/{cid}
		 */
		FileSource(const char* url);
		
		/**
		 * Create a file source. This is usually created from a search reply, 
		 * but might also be created from parsing a file list.
		 *
		 * @param hub      A hub URL, example: adc://host:port
		 * @param userID   A user ID, such as a CID (ADC) or nick (NMDC)
		 * @param filename Filename, might be 0 indicating the TTH root should be used.
		 *
		 * The filename might be set, if not set use TTH-sum based on the QueueFile.
		 * If the filename is "index" without a preceding "/", the file source
		 * interpreted as downloading the file list.
		 */
		FileSource(const char* hub, const char* userID, const char* filename = 0);
		
		virtual ~FileSource();
		
	public:
		
		/**
		 * @return the filename in utf-8, however might be 0 if no filename is known.
		 */
		inline const char* getName() const { return filename; }
		
		/**
		 * @return the Client/User ID where the file source is detected.
		 */
		inline const char* getClientID() const { return cid; }
		
		/**
		 * @return the URL to the hub where the user is connected to.
		 */
		inline const char* getHubUrl() const { return hub; }


	protected:
		char* hub;            // (adcs?|dchub)://(host(:port)?)
		char* cid;            // clientID (CID on ADC, nick on NMDC).
		char* filename;       // filename, full path, or NULL (tth is used instead).
		bool retryCount;      // expire this after ~50 tries
		Samurai::TimeStamp lastSeen; // expire this after ~30 days.
};

}

#endif // HAVE_QUICKDC_FILESOURCE_H
