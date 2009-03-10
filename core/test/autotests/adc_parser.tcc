#include <samurai/samurai.h>
#include <string.h>

#include "network/adc/parser.h"
#include "network/adc/cid.h"

EXO_TEST(adc_unescape_1, {
	char* buf = new char[100];
	buf[0] = 0;
	strcat(buf, "a\\sb");
	
	// ADC::Command cmd;
	char* res = ADC::Parser::unescape(&buf, strlen(buf));
	bool ok = strcmp(res, "a b") == 0;
	delete[] buf;	
	return  ok;
});

EXO_TEST(adc_unescape_2, {
	char* buf = new char[100];
	buf[0] = 0;
	strcat(buf, "a\\nb");
	
	// ADC::Command cmd;
	char* res = ADC::Parser::unescape(&buf, strlen(buf));
	bool ok = strcmp(res, "a\nb") == 0;
	delete[] buf;	
	return  ok;
});

EXO_TEST(adc_unescape_3, {
	char* buf = new char[100];
	buf[0] = 0;
	strcat(buf, "HELLO\\\\\\\\");
	
	// ADC::Command cmd;
	char* res = ADC::Parser::unescape(&buf, strlen(buf));
	bool ok = strcmp(res, "HELLO\\\\") == 0;
	delete[] buf;	
	return  ok;
});

EXO_TEST(adc_escape_1, {
	char buf[100];
	const char* in = "Jan Vidar Krey";
	ADC::Parser::escape(in, strlen(in), buf, 100);
	bool ok = strcmp(buf, "Jan\\sVidar\\sKrey") == 0;
	return  ok;
});

EXO_TEST(adc_escape_2, {
	char buf[100];
	const char* in = "A\nB\\O K";
	ADC::Parser::escape(in, strlen(in), buf, 100);
	bool ok = strcmp(buf, "A\\nB\\\\O\\sK") == 0;
	return  ok;
});

EXO_TEST(adc_cid_1, {
	return  ADC::CID::verify("M6CW4G5RICVGE67E4YDA6A3GX2EUZ2YEZA4S64I"); /* should be ok */
});

EXO_TEST(adc_cid_2, {
	return !ADC::CID::verify("M6CW4G5RICVGE67E4YDA6A3GX2EUZ2YEZA4S64"); /* should NOT be ok, one character too short */
});

EXO_TEST(adc_cid_3, {
	return !ADC::CID::verify("M6CW4G5RICVGE67E4YDA6A3GX2EUZ2YEZA4S64IA"); /* should NOT be ok, one character too long */
});

EXO_TEST(adc_cid_4, {
	return !ADC::CID::verify("999999999999999999999999999999999999999"); /* should NOT be ok, illegal characters, correct length */
});

EXO_TEST(adc_sid_1, {
	return strcmp(ADC::SID::toString(0), "AAAA") == 0;
});

EXO_TEST(adc_sid_2, {
	return strcmp(ADC::SID::toString(1), "AAAB") == 0;
});

EXO_TEST(adc_sid_3, {
	return ADC::SID::fromString("AAAA") == 0;
});

EXO_TEST(adc_sid_4, {
	return ADC::SID::fromString("AAAB") == 1;
});

EXO_TEST(adc_sid_max_1, {
	return ADC::SID::fromString("7777") == (1024*1024)-1;
});

EXO_TEST(adc_sid_max_2, {
	return strcmp(ADC::SID::toString(1024*1024-1), "7777") == 0;
});

EXO_TEST(adc_sid_overflow, {
	return strcmp(ADC::SID::toString(1024*1024), "AAAA") == 0;
});

EXO_TEST(adc_sid_invalid, {
	return ADC::SID::fromString("9999") == 0;
});

EXO_TEST(adc_command_broadcast, {
	const char* buf = "BINF 1234 NItest";
	ADC::Command cmd;
	bool ok = ADC::Parser::parse(buf, strlen(buf), cmd);
        return ok && cmd.isCommandBroadcast();
});

EXO_TEST(adc_command_hubinfo, {
        const char* buf = "IINF NItest";
	ADC::Command cmd;
	bool ok = ADC::Parser::parse(buf, strlen(buf), cmd);
	return ok && cmd.isCommandHubInfo();
});

EXO_TEST(adc_command_direct, {
        const char* buf = "DMSG 1234 5678 test";
	ADC::Command cmd;
	bool ok = ADC::Parser::parse(buf, strlen(buf), cmd);
	return ok && cmd.isCommandDirect();
});

EXO_TEST(adc_command_echo, {
        const char* buf = "EMSG 1234 5678 test";
	ADC::Command cmd;
	bool ok = ADC::Parser::parse(buf, strlen(buf), cmd);
	return ok && cmd.isCommandEcho();
});

EXO_TEST(adc_command_feature, {
        const char* buf = "FMSG 1234 +TCP4 test";
	ADC::Command cmd;
        bool ok = ADC::Parser::parse(buf, strlen(buf), cmd);
        return ok && cmd.isCommandFeature();
});

EXO_TEST(adc_command_hub, {
        const char* buf = "HSUP ADBASE";
        ADC::Command cmd;
        bool ok = ADC::Parser::parse(buf, strlen(buf), cmd);
        return ok && cmd.isCommandHub();
});

EXO_TEST(adc_command_udp, {
        const char* buf = "URES ZOYOBUSFL5KZHQIP7V47MSCZC2F2WT5SDNMYKKY FN/path/file.txt SI51615616 SL3 TOASBIERHFGIZE672 TRLJG4PQGFS6JKREMCZYSCTZT2I5HM2N2UJSSHMYA TD7";
        ADC::Command cmd;
        bool ok = ADC::Parser::parse(buf, strlen(buf), cmd);
        return ok && cmd.isCommandUdp();
});

EXO_TEST(adc_command_client, {
        const char* buf = "CSUP ADBASE";
        ADC::Command cmd;
        bool ok = ADC::Parser::parse(buf, strlen(buf), cmd);
        return ok && cmd.isCommandClient();
});

