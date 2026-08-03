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

/*
 * A flag's value sits at [0] of what getArgument() hands back, because the two letter
 * name has already been stripped. Reading it at [1] is why the ME flag never took
 * effect, so these pin the offset rather than the feature.
 */
EXO_TEST(adc_command_flag_me, {
	const char* buf = "BMSG AAAA waves ME1";
	ADC::Command cmd;
	if (!ADC::Parser::parse(buf, strlen(buf), cmd)) return false;

	char* flag = cmd.getArgument("ME", 2, 1);
	return flag && flag[0] == '1';
});

EXO_TEST(adc_command_flag_richtext, {
	const char* buf = "BMSG AAAA **bold** RT1";
	ADC::Command cmd;
	if (!ADC::Parser::parse(buf, strlen(buf), cmd)) return false;

	char* flag = cmd.getArgument("RT", 2, 1);
	if (!flag || flag[0] != '1') return false;

	/* The body is still argument zero, and untouched by the flag. */
	return strcmp(cmd.getArgument(0), "**bold**") == 0;
});

EXO_TEST(adc_command_flag_richtext_absent, {
	const char* buf = "BMSG AAAA weighs\\s*about*\\s3kg";
	ADC::Command cmd;
	if (!ADC::Parser::parse(buf, strlen(buf), cmd)) return false;

	/* No flag means the asterisks are the sender's own. */
	if (cmd.getArgument("RT", 2, 1)) return false;

	return strcmp(cmd.getArgument(0), "weighs *about* 3kg") == 0;
});

EXO_TEST(adc_command_markdown_roundtrip, {
	/*
	 * A multi-line markdown payload has to survive as a single argument: the escaper
	 * turns its spaces and newlines into \s and \n, and the parser has to give back
	 * exactly what went in.
	 */
	const char* body = "> quoted\n```\n  code();\n```";

	ADC::Command out(FOURCC('B','M','S','G'), "AAAA");
	out.addArgument(body);
	out.addArgument("RT", "1");

	const char* wire = out.getRawCommand();
	if (!wire) return false;

	ADC::Command back;
	/* getSize() counts the trailing newline, which is the terminator, not content. */
	if (!ADC::Parser::parse(wire, out.getSize() - 1, back)) return false;

	char* flag = back.getArgument("RT", 2, 1);
	if (!flag || flag[0] != '1') return false;

	return strcmp(back.getArgument(0), body) == 0;
});

EXO_TEST(adc_command_hub, {
        const char* buf = "HSUP ADBASE";
        ADC::Command cmd;
        bool ok = ADC::Parser::parse(buf, strlen(buf), cmd);
        return ok && cmd.isCommandHub();
});

/*
 * Rich text over a hub is negotiated in SUP, so a hub's answer has to arrive with the
 * extension still among the others it lists. The flags are positional, and the one that
 * matters sits in the middle of them.
 */
EXO_TEST(adc_command_sup_richtext_hub, {
	const char* buf = "ISUP ADBASE ADTIGR AD" ADC_EXT_RICHTEXT " ADPING";
	ADC::Command cmd;
	if (!ADC::Parser::parse(buf, strlen(buf), cmd)) return false;

	bool found = false;
	for (size_t n = 0; cmd.getArgument(n); n++)
		if (strcmp(cmd.getArgument(n), "AD" ADC_EXT_RICHTEXT) == 0)
			found = true;

	return found && cmd.isCommandHubInfo();
});

EXO_TEST(adc_command_sup_richtext_hub_absent, {
	/* A hub that lists other extensions and not this one says nothing about rich text. */
	const char* buf = "ISUP ADBASE ADTIGR ADPING";
	ADC::Command cmd;
	if (!ADC::Parser::parse(buf, strlen(buf), cmd)) return false;

	for (size_t n = 0; cmd.getArgument(n); n++)
		if (strcmp(cmd.getArgument(n), "AD" ADC_EXT_RICHTEXT) == 0)
			return false;

	return true;
});

EXO_TEST(adc_command_sup_richtext_removed, {
	/* A hub that drops the extension later says so with RM rather than AD. */
	const char* buf = "ISUP RM" ADC_EXT_RICHTEXT;
	ADC::Command cmd;
	if (!ADC::Parser::parse(buf, strlen(buf), cmd)) return false;

	return strcmp(cmd.getArgument(0), "RM" ADC_EXT_RICHTEXT) == 0;
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


/*
 * A command carrying an embedded NUL used to desynchronise the cached copy from
 * cachedSize: the allocation was sized with strlen() and stopped at the NUL,
 * while cachedSize kept counting to buflen. The hub hands both to write(), so
 * everything between the two was read off the heap and broadcast to every other
 * user. Refuse the line instead - ADC has no use for a NUL in one.
 */
EXO_TEST(adc_command_embedded_nul, {
        const char buf[] = "BMSG AAAB hi\0AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
        ADC::Command cmd;
        return !ADC::Parser::parse(buf, sizeof(buf) - 1, cmd);
});

EXO_TEST(adc_command_embedded_nul_leading, {
        const char buf[] = "HS\0P AAAAAAAAAAAAAAAAAAAAAAAA";
        ADC::Command cmd;
        return !ADC::Parser::parse(buf, sizeof(buf) - 1, cmd);
});

/* cachedSize must describe the cached buffer, whatever the caller passed. */
EXO_TEST(adc_command_cached_size_matches, {
        const char* buf = "BMSG AAAB hello";
        ADC::Command cmd;
        if (!ADC::Parser::parse(buf, strlen(buf), cmd)) return false;

        const size_t size = cmd.getSize();
        const char* raw = cmd.getRawCommand();
        if (!raw || size != strlen(buf) + 1) return false;

        /* The wire form is the line plus its terminator, and nothing past it. */
        return raw[size - 1] == '\n' && strncmp(raw, buf, strlen(buf)) == 0;
});

/* An unterminated buffer must not be read beyond buflen. */
EXO_TEST(adc_command_unterminated_buffer, {
        char raw[16];
        memset(raw, 'Z', sizeof(raw));
        memcpy(raw, "BMSG AAAB hi", 12);

        ADC::Command cmd;
        if (!ADC::Parser::parse(raw, 12, cmd)) return false;

        return cmd.getSize() == 13 && cmd.getRawCommand()[12] == '\n';
});
