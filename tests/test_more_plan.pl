use strict;
use warnings;
use Test::More tests => 5;

# use Test::More tests => N; prints the "1..N" plan line immediately
# (verified against real Perl — before any other output), and all N
# tests below are run so the count matches (this file stays a clean
# pass; the plan-mismatch / exit-code formula was verified separately
# and by hand against real Perl, documented in this session's report
# rather than checked into the regression harness as a known-failing
# script).
ok(1, "first");
is(2 + 2, 4, "second");
pass("third");
like("abc", qr/b/, "fourth");
cmp_ok(5, '>', 1, "fifth");

done_testing();
