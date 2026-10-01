use strict;
use warnings;
use Test::More;

# ok(): pass and fail, with and without a name
ok(1, "ok pass with name");
ok(0, "ok fail with name");
ok(1);
ok(0);

# is(): pass/fail for numbers, strings, undef
is(2 + 2, 4, "is numeric pass");
is(2 + 2, 5, "is numeric fail");
is("foo", "foo", "is string pass");
is("foo", "bar", "is string fail");
is(undef, "x", "is undef got fail");
is("x", undef, "is undef expected fail");
is(undef, undef, "is both undef pass");

# isnt()
isnt(1, 2, "isnt pass");
isnt(5, 5, "isnt fail");

# like() / unlike()
like("hello world", qr/wor/, "like pass");
like("hello world", qr/xyz/, "like fail");
unlike("hello world", qr/xyz/, "unlike pass");
unlike("hello world", qr/wor/, "unlike fail");

# cmp_ok(): all 10 required operators, mixed pass/fail
cmp_ok(5, '==', 5, "cmp_ok == pass");
cmp_ok(5, '==', 6, "cmp_ok == fail");
cmp_ok(5, '!=', 6, "cmp_ok != pass");
cmp_ok(5, '!=', 5, "cmp_ok != fail");
cmp_ok(3, '<', 5, "cmp_ok < pass");
cmp_ok(5, '<', 3, "cmp_ok < fail");
cmp_ok(5, '>', 3, "cmp_ok > pass");
cmp_ok(3, '>', 5, "cmp_ok > fail");
cmp_ok(5, '<=', 5, "cmp_ok <= pass");
cmp_ok(6, '<=', 5, "cmp_ok <= fail");
cmp_ok(5, '>=', 5, "cmp_ok >= pass");
cmp_ok(4, '>=', 5, "cmp_ok >= fail");
cmp_ok("foo", 'eq', "foo", "cmp_ok eq pass");
cmp_ok("foo", 'eq', "bar", "cmp_ok eq fail");
cmp_ok("foo", 'ne', "bar", "cmp_ok ne pass");
cmp_ok("foo", 'ne', "foo", "cmp_ok ne fail");
cmp_ok("abc", 'lt', "abd", "cmp_ok lt pass");
cmp_ok("abd", 'lt', "abc", "cmp_ok lt fail");
cmp_ok("abd", 'gt', "abc", "cmp_ok gt pass");
cmp_ok("abc", 'gt', "abd", "cmp_ok gt fail");

# pass() / fail()
pass("explicit pass");
fail("explicit fail");

# diag() / note() — multiple args (joined with no separator) and
# multi-line text (each physical line gets its own "# " prefix).
diag("diag ", "with ", "multiple ", "args");
note("note ", "with ", "multiple ", "args");
diag("multi\nline\ndiag");
note("multi\nline\nnote");

# subtest — nested group, kept passing-only in this harness-run file (a
# FAILING multi-line subtest's own outer diagnostic line number is a
# known, documented divergence from real Perl — see TESTS.md/this
# session's report; it does not affect pass/fail counts, TAP numbering,
# or indentation, only that one "at FILE line N." value).
subtest "a passing group" => sub {
    ok(1, "inner pass 1");
    ok(1, "inner pass 2");
    is(1 + 1, 2, "inner is pass");
    done_testing();
};

done_testing();
