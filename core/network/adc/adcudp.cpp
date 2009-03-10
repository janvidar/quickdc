/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

ADC::DatagramHandler::DatagramHandler() {
	hub = QuickDC::Core::getInstance()->localHub;
}

ADC::DatagramHandler::~DatagramHandler() {

}

void ADC::DatagramHandler::onDatagram(Samurai::IO::Net::DatagramSocket* socket, Samurai::IO::Net::DatagramPacket* packet) {
	size_t length = packet->getBuffer()->size();
	char* command = packet->getBuffer()->memdup(0, length+1);
	
	ADC::Command* cmd = new ADC::Command();
	bool ok = ADC::Parser::parse(command, length, *cmd);
	cmd->incRef();
	// cmd->setSocket(socket);
	
	if (ok) {
		switch (cmd->
		
	}
	
	cmd->decRef();
	if (cmd->canDelete())
		delete cmd;
	
	free(command);
}
