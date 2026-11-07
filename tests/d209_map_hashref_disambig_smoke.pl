#!/usr/bin/perl
# D209 (2026-11-07): map +{ ... } — the leading `+` is Perl's
# block/hashref disambiguator. It forces a SINGLE hashref per element,
# NOT the key/value PAIRS a bare `{ k => v }` block yields. Because
# parseUnary's unary-plus is a no-op, the `+` had to be recorded on the
# map node so codegen pushes one scalar instead of extending by pairs.
use strict; use warnings;

my @list = (1, 2, 3, 4);

# single-hashref form (the D209 bug: previously double-counted -> 8)
my @single = map +{ a => $_ }, @list;
print "single=", scalar(@single), "\n";

# bare `{ k => v }` block in list context yields PAIRS (2 per element)
my @pairs = map { $_ => 1 } (@list);
print "pairs=", scalar(@pairs), "\n";

# double-brace `{ { ... } }` is already a single hashref
my @dbl = map { { b => $_ } } @list;
print "dbl=", scalar(@dbl), "\n";

# scalar context counts the result list
my $n = scalar(map +{ c => $_ }, @list);
print "scalarcount=", $n, "\n";
