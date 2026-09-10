#!/usr/bin/perl
# Smoke test: Data::Dumper was completely unimplemented — Dumper() was
# an unrecognized bareword call, silently producing no output at all.
use strict;
use warnings;
use Data::Dumper;

my $out = Dumper(42);
print $out;
print(($out eq "\$VAR1 = 42;\n") ? "ok\n" : "FAIL\n");
