/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADC_UDP_PARSER_H
#define HAVE_QUICKDC_ADC_UDP_PARSER_H

namespace Samurai {
	namespace IO {
		namespace Net {
			class DatagramSocket;
			class DatagramPacket;
		}
	}
}

namespace ADC {

	class Command;

	/**
	 * Handles the ADC datagrams a client receives.
	 *
	 * An active client gives the hub a UDP port, and peers answer its searches
	 * straight to it rather than back through the hub.
	 */
	class DatagramHandler {
		public:
			/**
			 * Parse a datagram and act on it. These are unsolicited and
			 * unauthenticated, so the sender is not to be trusted.
			 */
			static void onDatagram(Samurai::IO::Net::DatagramSocket* socket, Samurai::IO::Net::DatagramPacket* packet);

		/**
		 * Record that a datagram answering one of our searches arrived from
		 * somewhere other than this machine, which is the only thing that shows the
		 * peer port can be reached over UDP.
		 */
		static void noteReachable(Samurai::IO::Net::DatagramPacket* packet,
			ADC::Command* cmd);

		protected:
			/** URES: a peer answering one of our searches. */
			static void onSearchResult(ADC::Command* cmd);

			/** UPSR: what a peer holds of a file we are downloading. */
			static void onPartialStatus(ADC::Command* cmd);
	};
}

#endif // HAVE_QUICKDC_ADC_UDP_PARSER_H
