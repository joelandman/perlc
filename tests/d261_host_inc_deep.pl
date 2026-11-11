use strict;
use warnings;
# The sub comes from Pod::Escapes in the host privlib (no -I).
# Numeric entities take the chr/oct path. Named-entity hashes in that
# module are a separate lookup and are not what this test gates.
use Pod::Escapes qw(e2char e2charnum);
print "sub=", (defined &e2char ? 1 : 0), "\n";
print "num65=[", e2char("65"), "]\n";
print "n65=", e2charnum("65"), "\n";
print "hex=[", e2char("x41"), "]\n";
my $blank = e2char("");
print "blank=", (defined $blank ? "def" : "undef"), "\n";
my $missing = e2charnum("nosuch_d261");
print "missing=", (defined $missing ? "def" : "undef"), "\n";
