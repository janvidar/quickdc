#include <samurai/samurai.h>
#include <samurai/stdc.h>
#include <string.h>
#include <limits.h>

/* ------------------------------------------------------------------------- *
 * quickdc_atoi / quickdc_atoll / quickdc_atoull
 *
 * These decode fields that arrive from a hub or a peer, so they see plenty of
 * input that is not purely a number.
 * ------------------------------------------------------------------------- */

EXO_TEST(atoi_plain_numbers, {
	return quickdc_atoi("0") == 0
		&& quickdc_atoi("7") == 7
		&& quickdc_atoi("1411") == 1411
		&& quickdc_atoi("-1") == -1
		&& quickdc_atoi("-32768") == -32768
		&& quickdc_atoi("+42") == 42;
});

EXO_TEST(atoi_stops_at_the_first_non_digit, {
	/*
	 * The whole point: the old converter counted every non-digit anywhere as
	 * leading padding and then read to the end, so "2/4" came out as -6.
	 */
	return quickdc_atoi("2/4") == 2
		&& quickdc_atoi("0/3") == 0
		&& quickdc_atoi("12/40") == 12
		&& quickdc_atoi("3000 QDC123") == 3000
		&& quickdc_atoi("140 Some message") == 140;
});

EXO_TEST(atoi_rejects_what_is_not_a_number, {
	return quickdc_atoi("") == 0
		&& quickdc_atoi("abc") == 0
		&& quickdc_atoi("/4") == 0
		&& quickdc_atoi("TTH:1234") == 0
		&& quickdc_atoi(0) == 0;
});

EXO_TEST(atoi_saturates_instead_of_wrapping, {
	/* A hub can say anything; wrapping would turn a silly number into a
	   plausible one. */
	return quickdc_atoi("99999999999999999999") == INT_MAX
		&& quickdc_atoi("-99999999999999999999") == INT_MIN
		&& quickdc_atoi("2147483647") == INT_MAX;
});

EXO_TEST(atoll_handles_sizes_and_junk, {
	return quickdc_atoll("0") == 0
		&& quickdc_atoll("3145728") == 3145728
		&& quickdc_atoll("5368709120") == 5368709120LL
		&& quickdc_atoll("-1") == -1
		&& quickdc_atoll("183 2/4") == 183
		&& quickdc_atoll("abc") == 0
		&& quickdc_atoll(0) == 0;
});

EXO_TEST(atoull_handles_sizes_and_junk, {
	return quickdc_atoull("0") == 0
		&& quickdc_atoull("3145728") == 3145728ULL
		&& quickdc_atoull("5368709120") == 5368709120ULL
		&& quickdc_atoull("18446744073709551615") == 18446744073709551615ULL
		&& quickdc_atoull("262144 1/1") == 262144ULL
		&& quickdc_atoull("abc") == 0
		&& quickdc_atoull(0) == 0;
});

EXO_TEST(atoull_refuses_negative_sizes, {
	/*
	 * strtoull() would turn these into something enormous, which is far worse
	 * than zero for a value used as a size or a file offset.
	 */
	return quickdc_atoull("-1") == 0
		&& quickdc_atoull("-5368709120") == 0
		&& quickdc_atoull(" -7") == 0;
});

EXO_TEST(atoi_leading_whitespace, {
	return quickdc_atoi("  42") == 42
		&& quickdc_atoi("\t7") == 7
		&& quickdc_atoull("  262144") == 262144ULL;
});
