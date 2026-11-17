use lib 'tests/lib';

# :DEFAULT is exactly @EXPORT — foo, bar imported; baz (export_ok) is not.
# The returned values prove the right subs resolved (not just that a sub
# happened to exist).
use E::DefaultTag qw(:DEFAULT);
my $foo = eval { foo() } // "FAIL";
my $bar = eval { bar() } // "FAIL";
my $baz = eval { baz() } // "unim";
printf "default:%s|%s|%s\n", $foo, $bar, $baz;

# The :other tag expands to its full set — now baz is available too, while
# foo/bar remain from @EXPORT. Confirm all three resolve to their real subs.
my $fooo = eval { foo() } // "FAIL";
my $barr = eval { bar() } // "FAIL";
my $bazx = eval { baz() } // "unim";
printf "other-tag:%s|%s|%s\n", $fooo, $barr, $bazx;

# Bare no-list `use` imports the same @EXPORT set as :DEFAULT (foo, bar).
use E::DefaultTag;
my $foo2 = eval { foo() } // "FAIL";
my $bar2 = eval { bar() } // "FAIL";
my $baz2 = eval { baz() } // "unim";
printf "bareuse:%s|%s|%s\n", $foo2, $bar2, $baz2;

print "deep_done\n";
