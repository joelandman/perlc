use strict;
use warnings;
# Inherited method, and a method whose empty return is assigned
# through an intermediate list the way Pod::Simple builds errata.
package Base255;
sub closers { return() }
sub items { return ("a", "b", "c") }
package Mid255;
our @ISA = ("Base255");
package main;
my $o = bless {}, "Mid255";
my @c = $o->closers();
print "closers=", scalar(@c), "\n";
my @items = $o->items();
print "items=", join(",", @items), "\n";
my @wrapped = ($o->closers(), "tail");
print "wrapped=", join(",", map { defined($_) ? $_ : "U" } @wrapped), " n=", scalar(@wrapped), "\n";
sub take { my $n = scalar(@_); print "take=$n\n" }
take($o->closers());
take($o->items());
