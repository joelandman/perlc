#!/usr/bin/env perl
# Smoke test: Sys::Hostname — hostname() via default @EXPORT.
# perlc implements Sys::Hostname natively (no .pm inlined).
use strict;
use warnings;
use Sys::Hostname;

my $h = hostname();
print(($h && length($h)) ? "host=ok\n" : "host=FAIL\n");
print(($h !~ /[\r\n]/) ? "clean=ok\n" : "clean=FAIL\n");
print "smoke_done\n";