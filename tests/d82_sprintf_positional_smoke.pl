#!/usr/bin/perl
# Smoke: D82 — sprintf/printf %N$ positional args.
use strict;

printf("D82:%3\$s %1\$s\n", "a", "b", "c");
print sprintf("%2\$d %1\$s", "x", 42), "\n";
print "d82_sprintf_positional_smoke_done\n";
