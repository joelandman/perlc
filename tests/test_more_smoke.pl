use strict;
use warnings;
use Test::More;

ok(1, "one is true");
is(2 + 2, 4, "addition");
isnt(1, 2, "not equal");
like("hello world", qr/wor/, "like matches");
unlike("hello world", qr/xyz/, "unlike no match");
cmp_ok(5, '>', 3, "cmp_ok greater");
pass("explicit pass");
diag("a diag message");
note("a note message");

done_testing();
