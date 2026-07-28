#include <samurai/samurai.h>
#include <samurai/stdc.h>
#include <string.h>
#include <limits.h>

/* ------------------------------------------------------------------------- *
 * Samurai::Util::Convert::to_int32 / to_int64 / to_uint64
 *
 * These decode fields that arrive from a hub or a peer, so they see plenty of
 * input that is not purely a number.
 * ------------------------------------------------------------------------- */

EXO_TEST(atoi_plain_numbers, {
	return Samurai::Util::Convert::to_int32("0") == 0
		&& Samurai::Util::Convert::to_int32("7") == 7
		&& Samurai::Util::Convert::to_int32("1411") == 1411
		&& Samurai::Util::Convert::to_int32("-1") == -1
		&& Samurai::Util::Convert::to_int32("-32768") == -32768
		&& Samurai::Util::Convert::to_int32("+42") == 42;
});

EXO_TEST(atoi_stops_at_the_first_non_digit, {
	/*
	 * The whole point: the old converter counted every non-digit anywhere as
	 * leading padding and then read to the end, so "2/4" came out as -6.
	 */
	return Samurai::Util::Convert::to_int32("2/4") == 2
		&& Samurai::Util::Convert::to_int32("0/3") == 0
		&& Samurai::Util::Convert::to_int32("12/40") == 12
		&& Samurai::Util::Convert::to_int32("3000 QDC123") == 3000
		&& Samurai::Util::Convert::to_int32("140 Some message") == 140;
});

EXO_TEST(atoi_rejects_what_is_not_a_number, {
	return Samurai::Util::Convert::to_int32("") == 0
		&& Samurai::Util::Convert::to_int32("abc") == 0
		&& Samurai::Util::Convert::to_int32("/4") == 0
		&& Samurai::Util::Convert::to_int32("TTH:1234") == 0
		&& Samurai::Util::Convert::to_int32(0) == 0;
});

EXO_TEST(atoi_saturates_instead_of_wrapping, {
	/* A hub can say anything; wrapping would turn a silly number into a
	   plausible one. */
	return Samurai::Util::Convert::to_int32("99999999999999999999") == INT_MAX
		&& Samurai::Util::Convert::to_int32("-99999999999999999999") == INT_MIN
		&& Samurai::Util::Convert::to_int32("2147483647") == INT_MAX;
});

EXO_TEST(atoll_handles_sizes_and_junk, {
	return Samurai::Util::Convert::to_int64("0") == 0
		&& Samurai::Util::Convert::to_int64("3145728") == 3145728
		&& Samurai::Util::Convert::to_int64("5368709120") == 5368709120LL
		&& Samurai::Util::Convert::to_int64("-1") == -1
		&& Samurai::Util::Convert::to_int64("183 2/4") == 183
		&& Samurai::Util::Convert::to_int64("abc") == 0
		&& Samurai::Util::Convert::to_int64(0) == 0;
});

EXO_TEST(atoull_handles_sizes_and_junk, {
	return Samurai::Util::Convert::to_uint64("0") == 0
		&& Samurai::Util::Convert::to_uint64("3145728") == 3145728ULL
		&& Samurai::Util::Convert::to_uint64("5368709120") == 5368709120ULL
		&& Samurai::Util::Convert::to_uint64("18446744073709551615") == 18446744073709551615ULL
		&& Samurai::Util::Convert::to_uint64("262144 1/1") == 262144ULL
		&& Samurai::Util::Convert::to_uint64("abc") == 0
		&& Samurai::Util::Convert::to_uint64(0) == 0;
});

EXO_TEST(atoull_refuses_negative_sizes, {
	/*
	 * strtoull() would turn these into something enormous, which is far worse
	 * than zero for a value used as a size or a file offset.
	 */
	return Samurai::Util::Convert::to_uint64("-1") == 0
		&& Samurai::Util::Convert::to_uint64("-5368709120") == 0
		&& Samurai::Util::Convert::to_uint64(" -7") == 0;
});

EXO_TEST(atoi_leading_whitespace, {
	return Samurai::Util::Convert::to_int32("  42") == 42
		&& Samurai::Util::Convert::to_int32("\t7") == 7
		&& Samurai::Util::Convert::to_uint64("  262144") == 262144ULL;
});
