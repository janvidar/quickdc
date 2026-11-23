/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_MAPPINGPOLICY_H
#define HAVE_QUICKDC_MAPPINGPOLICY_H

#include <cstdint>

namespace QuickDC {

/**
 * Whether to hold a gateway mapping, from what the configuration wants and what
 * the measurements have shown.
 *
 * The configuration decides whether one is wanted. What this adds is evidence: a
 * gateway grants a mapping and reports success without knowing whether anything
 * upstream honours it, so only a measurement taken through the mapping can say it
 * helped. One that still says passive shows it did not, and a mapping nothing can
 * use is a hole in somebody's network for nothing - so it is let go, and not
 * asked for again until the configuration changes.
 *
 * Every start and stop of the gateway goes through apply(); nothing else asks.
 */
class MappingPolicy {
	public:
		/** The gateway, as far as the policy needs it. */
		class Gateway {
			public:
				virtual ~Gateway() { }
				virtual bool forwarded() const = 0;
				virtual bool busy() const = 0;
				virtual void start(uint16_t port) = 0;
				virtual void stop() = 0;
		};

		/**
		 * Bring the gateway in line: ask for a mapping of 'port' where one is
		 * wanted and none is held or asked for, let go of one that is not wanted
		 * or is for another port.
		 *
		 * @return whether an ask is now under way, which is what a measurement
		 *         should wait for.
		 */
		bool apply(Gateway& gateway, bool wanted, uint16_t port);

		/** The configuration changed: a mapping that did not help gets another chance. */
		void configurationChanged() { useless = false; }

		/** The gateway granted the mapping; only checks started from now test it. */
		void granted() { tested = false; }

		/** The gateway refused, or nothing answered. */
		void refused() { tested = false; }

		/** A check started. Only the IPv4 run goes through the gateway. */
		void checkStarted(const Gateway& gateway, bool ipv6)
		{
			if (!ipv6 && gateway.forwarded()) tested = true;
		}

		/**
		 * A verdict of passive. Measured through the mapping, it shows the mapping
		 * does not help; with none held, one is worth asking for again where wanted.
		 */
		bool passive(Gateway& gateway, bool wanted, uint16_t port);

		bool isUseless() const { return useless; }

	private:
		uint16_t mapped = 0;
		bool tested = false;
		bool useless = false;
};

}

#endif // HAVE_QUICKDC_MAPPINGPOLICY_H
