#!/usr/bin/perl
# Smoke test for D113: calling an undefined sub must die (real Perl:
# "Undefined subroutine &main::nosuchsub called ...", exit 255) instead
# of silently returning undef. This was the mechanism by which three
# completely-unimplemented modules (Getopt::Long/Data::Dumper/
# File::Basename) went unnoticed until a human manually diffed output.
# No stdout output here at all (deliberately) — perlc's runtime doesn't
# honor $|=1 autoflush the same way real Perl's stdout buffering does,
# so mixing stdout prints with the stderr die message makes interleaving
# order non-deterministic between the two; the die message itself is
# the only thing under test.
use strict;
use warnings;

nosuchsub_d113(1, 2, 3);
print "unreachable\n";
