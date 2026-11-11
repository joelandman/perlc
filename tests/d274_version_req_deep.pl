#!/usr/bin/perl
# D274: object invocant, and a string $VERSION compared as a number.
use strict;
use warnings;

package F;
our $VERSION = 1.2;
my $o = bless {}, "F";
my $ok = eval { $o->VERSION(1.2) };
print "obj=$ok\n";
eval { $o->VERSION(3) };
my $bad = $@;
$bad =~ s/ at .*//;
print "objbad=$bad";

package I;
our $VERSION = "1.10";
eval { I->VERSION("1.2") };
my $s = $@;
$s =~ s/ at .*//;
print "str=$s";
eval { I->VERSION("1.10") };
my $same = $@;
$same =~ s/ at .*//;
print "same=", ($same eq "" ? "empty" : $same), "\n";
my $got = eval { I->VERSION("1.0") };
print "got=$got\n";
