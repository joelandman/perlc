#!/usr/bin/perl
# D217: symmetric qw delimiters, nested brackets, and a later same character.
use strict;
use warnings;
my @slash = qw/text TEXT pod/;
print "slash=", join("|", @slash), " n=", scalar(@slash), "\n";
my @empty = qw//;
print "empty=", scalar(@empty), "\n";
my @bang = qw!one two!;
print "bang=", join("|", @bang), "\n";
my @hashd = qw#x y#;
print "hashd=", join("|", @hashd), "\n";
my @br = qw[a[b]c d];
print "br=", join("|", @br), "\n";
my @pa = qw(a(b)c d);
print "pa=", join("|", @pa), "\n";
my @an = qw<a<b>c>;
print "an=", join("|", @an), "\n";
my $sum = qw/a/ . "X";
print "ax=$sum\n";
my @sp = qw /spaced words/;
print "sp=", join("|", @sp), "\n";
my @nl = qw/
  alpha
  beta
/;
print "nl=", join("|", @nl), "\n";
my @esc = qw/a\/b c/;
print "esc=", join("|", @esc), "\n";
sub take { return join(",", @_) }
print "call=", take(qw/p q/), "\n";
print "after\n";
