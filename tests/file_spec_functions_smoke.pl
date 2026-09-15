#!/usr/bin/env perl
# Smoke test: File::Spec::Functions — the functional API via default @EXPORT
# and explicit imports (real File::Spec::Functions only default-exports
# canonpath/catdir/catfile/curdir/rootdir/updir/no_upwards/file_name_is_absolute/
# path; splitpath/splitdir/catpath/abs2rel/rel2abs/devnull/tmpdir need
# qw(...) or :ALL).
use strict;
use warnings;
use File::Spec::Functions;

print catfile("a","b"), "\n";
print canonpath("a//b"), "\n";
print curdir(), "\n";
print rootdir(), "\n";
print updir(), "\n";
print file_name_is_absolute("/x") ? "abs\n" : "rel\n";
print "smoke_done\n";