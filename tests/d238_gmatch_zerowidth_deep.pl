#!/usr/bin/perl
# D238: zero-width /g advances one character, reports pos at the match
# end (including 0), and a failed match clears pos. A start past the
# end fails. Ordinary consuming /g is unchanged.
use strict;
use warnings;

my $s = "bbb";
my $n = 0;
while ($s =~ /(a*)/g) {
    my $p = defined(pos($s)) ? pos($s) : "U";
    print "z pos=$p len=", length($1), "\n";
    last if ++$n > 10;
}
print "z n=$n end=", (defined(pos($s)) ? pos($s) : "U"), "\n";

$s = "foo";
$n = 0;
while ($s =~ m/([^\n\r]*)(\r?\n?)/g) {
    my $p = defined(pos($s)) ? pos($s) : "U";
    print "plain 1=[$1] pos=$p\n";
    last if ++$n > 8;
}
print "plain n=$n end=", (defined(pos($s)) ? pos($s) : "U"), "\n";

$s = "aXaX";
my @m;
while ($s =~ /X/g) { push @m, pos($s); }
print "Xs=", join(",", @m), " end=", (defined(pos($s)) ? pos($s) : "U"), "\n";

$s = "ab";
pos($s) = 5;
print(($s =~ /a/g) ? "hit\n" : "miss\n");
print "past=", (defined(pos($s)) ? pos($s) : "U"), "\n";

# A consuming match after a zero-width run still sees the next character.
$s = "xa";
$n = 0;
while ($s =~ /a?/g) {
    my $p = defined(pos($s)) ? pos($s) : "U";
    my $m = defined($&) ? $& : "";
    print "q=[$m] pos=$p\n";
    last if ++$n > 8;
}
print "q n=$n end=", (defined(pos($s)) ? pos($s) : "U"), "\n";
