/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADC_UDP_PARSER_H
#define HAVE_QUICKDC_ADC_UDP_PARSER_H

namespace ADC {

	class DatagramHandler {
		public:
			DatagramHandler();
			virtual ~DatagramHandler();
	
			/**
			 * This method will handle any incoming UDP packets destined for the hub.
			 */
			virtual void onDatagram(Samurai::IO::Net::DatagramSocket* socket, Samurai::IO::Net::DatagramPacket* packet);
			
			
		protected:
			ADC::Hub* hub;
	
	};
}




#endif // HAVE_QUICKDC_ADC_UDP_PARSER_H

