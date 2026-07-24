#!/usr/bin/perl
# Smoke: D46 — ternary without spaces around :
my $x = 1; my $y = 2;
print "a=", ($x?$y:"str"), "\n";
print "b=", (0?$y:"z"), "\n";
print "ok\n";
