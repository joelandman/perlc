#!/usr/bin/perl
# D235: /r with one match, no match, and a count-returning s/// still
# leaves the original changed only when /r is absent.
use strict;
use warnings;
my $s = "xx-yy-zz";
my $one = $s =~ s/-/:/r;
print "one=$one s=$s\n";
my $all = $s =~ s/-/:/gr;
print "all=$all s=$s\n";
my $n = $s =~ s/-/:/g;
print "n=$n s=$s\n";
my $plain = "keep";
my $same = $plain =~ s/nope/Z/r;
print "same=$same plain=$plain\n";
my $layer = "PerlIO::utf8";
my $path = $layer =~ s{::}{/}gr . ".pm";
print "path=$path layer=$layer\n";
my $left = "ab" . "cd" =~ s/b/X/r;
print "left=$left\n";
my $cat = "ab" =~ s/b/X/r . "cd";
print "cat=$cat\n";
