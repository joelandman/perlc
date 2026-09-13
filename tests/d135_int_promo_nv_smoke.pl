#!/usr/bin/perl
# Smoke test for D135: a sub-scope `my $x = 0;` (int-promoted to an unboxed
# i64 alloca) used to silently truncate any later fractional NV assignment
# — `$x = 5.5` held 5 — because every subsequent Assign routed through the
# int-var branch, whose boxed fallback perl_to_ints the value. Real Perl has
# no sticky per-variable type. Int-promotion is now refused when the body
# ever assigns the name a not-statically-int RHS.
use strict;
use warnings;

sub f { my $acc = 0; $acc = 5.5; return $acc; }
print f(), "\n";
print "d135_done\n";