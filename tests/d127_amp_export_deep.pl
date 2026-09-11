#!/usr/bin/perl
# Deep test for D127: scanExports() (src/main.cpp) stored a qw()-listed
# export name with its leading &/* sigil intact instead of stripping it
# — real Pod::Usage.pm's `our @EXPORT = qw(&pod2usage);` is the exact
# real-world shape (found via a 2026-09-10 module-compile survey). This
# broke BOTH the default-@EXPORT bare-`use` path (importMap never got
# populated under the plain name) and an explicit `use Module
# qw(name)` import (D26's exported-name validation compared "name"
# against the stored "&name" and rejected it).
use strict;
use warnings;
use lib 'tests/lib';
# bare use (no explicit list) — pulls in the default @EXPORT (greet)
use D127AmpExport;
# explicit @EXPORT_OK import — separate fixture with no @EXPORT of its
# own, so this doesn't interact with D127AmpExport's default export
use D127AmpExportOk qw(shout);

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# default @EXPORT (bare use, no explicit list) — greet() unqualified
check('default_export_unqualified', greet() eq "hello");
check('default_export_qualified', D127AmpExport::greet() eq "hello");

# @EXPORT_OK member not requested from D127AmpExport — real Exporter
# semantics: only reachable qualified unless explicitly imported (an
# explicit import list would replace, not add to, the default @EXPORT
# above, so this is deliberately checked via a qualified call instead).
check('export_ok_qualified', D127AmpExport::farewell() eq "bye");

# explicit @EXPORT_OK import (separate fixture, no @EXPORT of its own)
check('explicit_import_unqualified', shout() eq "SHOUT");
check('explicit_import_qualified', D127AmpExportOk::shout() eq "SHOUT");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d127_amp_export_done\n";
