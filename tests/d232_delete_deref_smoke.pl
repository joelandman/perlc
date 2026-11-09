#!/usr/bin/perl
# D232: delete $$ref{key} / delete $$ref[idx] is one deref.
use strict;
use warnings;
my $h = { a => 1, b => 2, c => 3 };
my $r = delete $$h{b};
print "del=$r\n";
print "a=$$h{a}\n";
print defined($$h{b}) ? "bset\n" : "bgone\n";
my $miss = delete $$h{nope};
print defined($miss) ? "miss=$miss\n" : "missundef\n";
delete $$h{a};
print defined($$h{a}) ? "aset\n" : "agone\n";
my $k = delete($h->{c});
print "k=$k\n";
my $a = [10, 20, 30];
my $e = delete $$a[1];
print "e=$e\n";
print "0=$$a[0]\n";
print defined($$a[1]) ? "1set\n" : "1gone\n";
print "2=$$a[2]\n";
