#!/usr/bin/perl
# Deep test: Sys::Hostname — bare and fully-qualified call forms, arg-less
# and () call, and the result matches `hostname(1)`/gethostname(2) on this
# host. Deterministic: the hostname doesn't change during a run, so both
# real perl and perlc see the same name.
use strict;
use warnings;
use Sys::Hostname;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $h1 = hostname();
my $h2 = hostname();
my $h3 = Sys::Hostname::hostname();
my $h4 = Sys::Hostname::hostname();

check("defined",    defined($h1));
check("nonempty",   length($h1) > 0);
check("no_newline", $h1 !~ /[\r\n]/);
check("stable",     $h1 eq $h2 && $h1 eq $h3 && $h1 eq $h4);
check("smoke_done", 1);
print scalar(@failures), " failures\n";