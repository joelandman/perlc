#!/usr/bin/perl
# Regression guard (perlc D189) for the `undef EXPR` lvalue-clearing
# behavior: `undef $s` undefines the scalar, `undef @a`/`undef %h` empties
# the container (which is still defined). Output must match real perl
# byte-for-byte.
use strict;
use warnings;

my $x = 42;
undef $x;
print "scalar: ", defined($x) ? "defined" : "undef", "\n";

my @a = (1, 2, 3);
undef @a;
print "array_len: ", scalar(@a), "\n";

my %h = (k => "v");
undef %h;
print "hash_len: ", scalar(keys %h), "\n";

my $acc = "start";
undef $acc;
$acc .= "after";
print "reinit: [$acc]\n";

print "d189_undef_done\n";
