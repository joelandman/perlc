#!/usr/bin/perl
# Smoke test for D122: scanExports() must recognize the `use vars
# qw(@EXPORT_OK); @EXPORT_OK = qw(...)` style (no `our` prefix) that
# real core File::Path.pm ships with, not just `our @EXPORT_OK = ...`.
use strict;
use warnings;
use lib 'tests/lib';
use D122UseVarsExport qw(explicitly_imported);

print explicitly_imported(), "\n";
