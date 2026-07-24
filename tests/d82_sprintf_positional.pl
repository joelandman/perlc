#!/usr/bin/perl
# Deep: D82 — positional sprintf/printf args.
use strict;

print "=== basic swap ===\n";
printf("[%3\$s][%1\$s]\n", "a", "b", "c");
print sprintf("[%2\$d][%1\$s]", "x", 42), "\n";

print "=== mixed positional + sequential ===\n";
printf("[%2\$s][%s][%s]\n", "a", "b", "c");

print "=== positional width *N\$ ===\n";
printf("[%*2\$s]\n", "hi", 5);

print "=== literal percent ===\n";
print sprintf("%% %1\$s %%", "ok"), "\n";

print "=== sequential still works ===\n";
print sprintf("%s-%s", "a", "b"), "\n";
printf("%d %d\n", 1, 2);

print "d82_sprintf_positional_done\n";
