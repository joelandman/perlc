#!/usr/bin/perl
# D209 deep: the +{...} hashref disambiguator across contexts and nesting.
use strict; use warnings;

my @list = (1, 2, 3);

# the disambiguator applies to any +{...}, not just a=>$_
my @refs = map +{ n => $_, tag => "t$_" }, @list;
print "refs=", scalar(@refs), "\n";

# assigning the result list to @ (list context) still gets one ref each
my @assigned = map +{ v => $_ }, @list;
print "assigned=", scalar(@assigned), "\n";

# nested map: outer yields refs whose inner map also yields single refs
my @nested = map { +{ outer => $_, inner => [ map +{ $_ => 1 }, @list ] } } @list;
print "nested=", scalar(@nested), "\n";

# map +{...} feeding a scalar-count in list-then-scalar
my @big = (10, 20, 30, 40, 50);
my $count = scalar(@{ [ map +{ x => $_ }, @big ] });
print "count=", $count, "\n";

# bare { k => v } (no +) still yields PAIRS in list context -> twice as many
my @barepairs = map { $_ => $_ * 10 } (@list);
print "barepairs=", scalar(@barepairs), "\n";

# the +{...} must NOT flatten the hash into its pairs
my @noflatten = map +{ one => 1, two => 2, three => 3 }, @list;
print "noflatten=", scalar(@noflatten), "\n";
