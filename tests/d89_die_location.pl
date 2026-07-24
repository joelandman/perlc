#!/usr/bin/perl
# Deep: D89 — die location suffix
print "=== no newline ===\n";
eval { die "boom" };
print "e=$@";
print "=== with newline ===\n";
eval { die "done\n" };
print "e=$@";
print "=== empty-ish ===\n";
eval { die "x" };
print "has_at=", ($@ =~ / at / ? "yes" : "no"), "\n";
print "has_line=", ($@ =~ / line \d+\./ ? "yes" : "no"), "\n";
print "d89_die_location_done\n";
