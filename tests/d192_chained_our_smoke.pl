#!/usr/bin/perl
# Regression guard (perlc D192) for a chained `our` used as a chainable
# assignment EXPRESSION: `our $a = our $b = ...;` (real Perl makes `our`
# return the assigned value, so the chain binds left-to-right). This is the
# exact idiom /usr/bin/splain uses at file scope:
#   our $opt_p = our $opt_d = our $opt_v = our $opt_f = '';
# Output must match real perl byte-for-byte.
use strict;
use warnings;

sub check { my ($name, $ok) = @_; print $name, "=", ($ok ? "ok" : "FAIL"), "\n"; }

our $a = our $b = 5;
check('two_var_scalar', $a == 5 && $b == 5);

our $x = our $y = our $z = 10;
check('three_var_scalar', $x == 10 && $y == 10 && $z == 10);

our $p = our $q = (2 + 3);
check('chained_our_expr', $p == 5 && $q == 5);

check('d192_chained_our_done', 1);
