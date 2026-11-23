/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/mappingpolicy.h"

#include "quickdc.h"

bool QuickDC::MappingPolicy::apply(Gateway& gateway, bool wanted, uint16_t port)
{
	const bool want = wanted && !useless;
	const bool held = gateway.forwarded() || gateway.busy();

	if (held && (!want || mapped != port))
	{
		gateway.stop();
		tested = false;
		mapped = 0;
	}

	if (want && !(gateway.forwarded() || gateway.busy()))
	{
		mapped = port;
		gateway.start(port);
	}

	return gateway.busy();
}

bool QuickDC::MappingPolicy::passive(Gateway& gateway, bool wanted, uint16_t port)
{
	if (gateway.forwarded() && tested)
	{
		QDBG("The mapping did not help; releasing it");
		useless = true;
	}

	return apply(gateway, wanted, port);
}
