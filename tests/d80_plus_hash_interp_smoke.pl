#!/usr/bin/perl
# Smoke: D80 — $+{name} interpolates inside double-quoted strings.
use strict;

"2020-01" =~ /(?<y>\d+)-(?<m>\d+)/;
print "interp:$+{y}-$+{m}\n";
print "bare:", $+{y}, "\n";
print "d80_plus_hash_interp_smoke_done\n";
