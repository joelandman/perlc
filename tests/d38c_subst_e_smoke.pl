#!/usr/bin/perl
# Smoke: D38c — s///e evaluates replacement as code
my $x = "hello";
$x =~ s/l/uc($&)/e;
die "uc" unless $x eq "heLlo";
my $y = "a1b2";
$y =~ s/(\d)/$1+1/ge;
die "ge" unless $y eq "a2b3";
print "ok\n";
