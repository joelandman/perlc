#!/usr/bin/perl
# Smoke: POD between =pod/=cut is ignored.
use strict;
use warnings;

=pod

This is documentation, not code.
print "SHOULD_NOT_RUN\n";
sub fake { }

=cut

print "pod_ok\n";

=head1 NAME

more pod

=cut

print "pod_skip_smoke_done\n";
