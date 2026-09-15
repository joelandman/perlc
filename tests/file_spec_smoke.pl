#!/usr/bin/env perl
# Smoke test: File::Spec — catfile/catdir/canonpath basics via class methods.
# perlc implements File::Spec::Unix natively (no .pm inlined); this test
# pins the exact Unix outputs real perl produces on this OS.
use strict;
use warnings;
use File::Spec;

my $p = File::Spec->catfile("a", "b", "c.pl");
print(($p eq "a/b/c.pl") ? "catfile=ok\n" : "catfile=FAIL [$p]\n");
my $d = File::Spec->catdir("a", "b");
print(($d eq "a/b") ? "catdir=ok\n" : "catdir=FAIL [$d]\n");
print "smoke_done\n";