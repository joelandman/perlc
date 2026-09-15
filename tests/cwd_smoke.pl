#!/usr/bin/env perl
# Smoke test: Cwd — getcwd()/cwd() bare calls (default @EXPORT).
# perlc implements Cwd natively (no .pm inlined); abs_path needs an
# explicit import list in real perl too, so it's tested in the deep test.
use strict;
use warnings;
use Cwd;

my $cwd = getcwd();
print(($cwd =~ m{^/}) ? "cwd=ok\n" : "cwd=FAIL\n");
my $cwd2 = cwd();
print(($cwd2 eq $cwd) ? "cwd2=ok\n" : "cwd2=FAIL\n");
print "smoke_done\n";