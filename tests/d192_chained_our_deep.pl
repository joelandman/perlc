#!/usr/bin/perl
# Deep regression guard (perlc D192) for chained `our` used as a chainable
# assignment EXPRESSION (`our $a = our $b = ...;`), exercising the shapes a
# real program hits: the exact /usr/bin/splain file-scope four-way chain
# (`our $opt_p = our $opt_d = our $opt_v = our $opt_f = '';`), a chain with an
# expression RHS, and a chain scoped inside a sub. Scalar `our` only is
# covered (array/hash/list chained-our is a separate, pre-existing gap).
# Output must match real perl byte-for-byte.
use strict;
use warnings;

sub check { my ($name, $ok) = @_; print $name, "=", ($ok ? "ok" : "FAIL"), "\n"; }

# the exact /usr/bin/splain file-scope four-way chain (D192's repro)
our $opt_p = our $opt_d = our $opt_v = our $opt_f = '';
check('splain_four_way', !($opt_p || $opt_d || $opt_v || $opt_f));
$opt_p = 'p'; $opt_v = 'v';
check('splain_four_way_write', $opt_p eq 'p' && $opt_v eq 'v');

# chained our with an expression RHS binds right-to-left through the chain
our $m = our $n = our $k = (6 * 7);
check('chained_our_expr', $m == 42 && $n == 42 && $k == 42);

# chained our scoped inside a sub
sub opts {
    our $l = our $j = our $i = 100;
    return ($i, $j, $l);
}
my @v = opts();
check('chained_our_in_sub', "@v" eq "100 100 100");

check('d192_chained_our_done', 1);
