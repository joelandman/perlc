#!/usr/bin/perl
# Smoke test for D127: an export name with a leading & sigil in the
# qw() list (e.g. real Pod::Usage.pm's `our @EXPORT = qw(&pod2usage);`)
# must still match a plain unqualified call/import — the & is a marker,
# not part of the symbol name.
use strict;
use warnings;
use lib 'tests/lib';
use D127AmpExport;

print greet(), "\n";
