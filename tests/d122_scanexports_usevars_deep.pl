#!/usr/bin/perl
# Deep test for D122: `use vars`-declared @EXPORT/@EXPORT_OK (no `our`
# prefix) are correctly recognized by scanExports(). An explicit import
# list (qw(explicitly_imported)) replaces @EXPORT rather than adding to
# it (real Exporter semantics — see D26 in TESTS.md), so
# always_exported() is checked only via its fully-qualified name here,
# not unqualified.
use strict;
use warnings;
use lib 'tests/lib';
use D122UseVarsExport qw(explicitly_imported);

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

check('explicit_import_unqualified', explicitly_imported() eq "explicit");
check('explicit_import_qualified', D122UseVarsExport::explicitly_imported() eq "explicit");
check('default_export_qualified', D122UseVarsExport::always_exported() eq "always");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d122_scanexports_usevars_done\n";
