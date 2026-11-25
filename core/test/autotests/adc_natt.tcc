/*
 * ADC::peerRole: which end of a peer connection is the TLS server and which
 * opens the C-C handshake, including a NATT connection that both ends dialled.
 */

#include "network/adc/adccommon.h"

EXO_TEST(adc_peer_role_plain_dial, {
	const ADC::PeerRole out = ADC::peerRole(ADC::PeerDial::Out);
	const ADC::PeerRole in = ADC::peerRole(ADC::PeerDial::In);
	return !out.tlsServer && out.speaksFirst && in.tlsServer && !in.speaksFirst;
});

EXO_TEST(adc_peer_role_dnat_sender_is_dialled, {
	const ADC::PeerRole role = ADC::peerRole(ADC::PeerDial::SentNat);
	return role.tlsServer && !role.speaksFirst;
});

EXO_TEST(adc_peer_role_drnt_sender_dials, {
	const ADC::PeerRole role = ADC::peerRole(ADC::PeerDial::SentRnt);
	return !role.tlsServer && role.speaksFirst;
});

EXO_TEST(adc_peer_role_traversal_ends_differ, {
	/* Whichever way round the traversal went, the two ends never pick the same. */
	const ADC::PeerRole nat = ADC::peerRole(ADC::PeerDial::SentNat);
	const ADC::PeerRole rnt = ADC::peerRole(ADC::PeerDial::SentRnt);
	return nat.tlsServer != rnt.tlsServer && nat.speaksFirst != rnt.speaksFirst
		&& nat.tlsServer != nat.speaksFirst;
});
