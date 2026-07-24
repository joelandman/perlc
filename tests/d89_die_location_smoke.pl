#!/usr/bin/perl
# Smoke: D89 — die without trailing newline gets " at FILE line N."
eval { die "no newline" };
my $e = $@;
die "missing suffix" unless $e =~ /no newline at .* line \d+\.\n\z/;
die "quoted path" if $e =~ /at "/;
eval { die "has newline\n" };
die "kept newline" unless $@ eq "has newline\n";
print "ok\n";
