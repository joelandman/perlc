#!/usr/bin/perl
# Smoke: D49 — $SIG{__WARN__} intercepts warn()
my @w;
$SIG{__WARN__} = sub { push @w, $_[0] };
warn "hello";
die "count" unless @w == 1;
die "msg" unless $w[0] =~ /^hello at /;
print "ok\n";
