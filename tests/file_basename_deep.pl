#!/usr/bin/perl
# Deep: File::Basename — found completely unimplemented during a
# real-world code survey (2026-09): basename()/dirname() silently
# returned undef (empty string) instead of erroring, a common source of
# "works with real perl, silently wrong with perlc" path-manipulation
# bugs.
#
# Implementation: src/runtime.c perl_basename/perl_dirname/perl_fileparse
# (with a shared fb_strip_trailing_slashes/fb_strip_suffix), dispatched
# from src/codegen.cpp. fileparse has both a list-context case
# (emitArrayPtr, returns the full (name,path,suffix) triple) and a
# scalar-context case (emitExpr, returns just the name) — mirroring how
# localtime/stat already handle the two contexts differently.
#
# NOT supported: a qr// suffix argument (e.g.
# fileparse($path, qr/\.[^.]*/), a real File::Basename idiom found in
# /usr/bin/ptardiff on this system) — qr// isn't implemented as a value
# at all yet, a separate, larger missing feature. A non-string suffix
# argument is silently skipped rather than crashing.
use strict;
use warnings;
use File::Basename;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

check('basename_simple', basename("/usr/local/bin/foo.txt") eq "foo.txt");
check('basename_suffix_stripped',
    basename("/usr/local/bin/foo.txt", ".txt") eq "foo");
check('basename_trailing_slash', basename("/usr/local/bin/") eq "bin");
check('basename_no_dir', basename("foo.txt") eq "foo.txt");

check('dirname_simple', dirname("/usr/local/bin/foo.txt") eq "/usr/local/bin");
check('dirname_no_trailing_component', dirname("/usr/local/bin") eq "/usr/local");
check('dirname_no_dir', dirname("foo.txt") eq ".");
check('dirname_root', dirname("/") eq "/");
check('dirname_dot', dirname(".") eq ".");

{
    my ($name, $path, $suffix) = fileparse("/usr/local/bin/foo.txt");
    check('fileparse_list_context',
        $name eq "foo.txt" && $path eq "/usr/local/bin/" && $suffix eq "");
}
{
    my ($name, $path, $suffix) =
        fileparse("/usr/local/bin/foo.txt", ".txt", ".dat");
    check('fileparse_with_suffixes',
        $name eq "foo" && $path eq "/usr/local/bin/" && $suffix eq ".txt");
}
{
    my ($name, $path, $suffix) = fileparse("foo.txt");
    check('fileparse_no_dir',
        $name eq "foo.txt" && $path eq "./" && $suffix eq "");
}
{
    my $name_only = fileparse("/a/b/foo.txt");
    check('fileparse_scalar_context', $name_only eq "foo.txt");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "file_basename_deep_done\n";
