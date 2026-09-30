#!/usr/bin/perl
# Deep test for D158: `split PATTERN` with no explicit 2nd argument
# must split $_ (real Perl's documented default), regardless of
# delimiter style (`m{...}`, `/.../`) or the pattern being a plain
# string. src/codegen.cpp's two NK::SplitFunc sites (emitArrayPtr's
# list-context copy and emitExpr's scalar/general copy — a duplicate-
# codegen-site pattern this codebase has hit before, e.g. D115's
# emitBlockLast/emitStmt Return duplication) both unconditionally used
# perlUndef() when the 2nd argument was omitted, instead of looking up
# $_ the same way print/say/chomp's own no-args case already does.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

{
    local $_ = "a/b/c";
    my @r = split m{/};
    check('brace_delim_implicit', join(",", @r) eq "a,b,c");
}
{
    local $_ = "a/b/c";
    my @r = split /\//;
    check('slash_delim_implicit', join(",", @r) eq "a,b,c");
}
{
    local $_ = "a:b:c";
    my @r = split ":";
    check('plain_string_pattern_implicit', join(",", @r) eq "a,b,c");
}
# inside a map block — $_ is the loop variable, not a lexical
{
    my @list = ("/tmp/foo/bar.txt", "/tmp/x/y.dat");
    my @r = map { (split m{/})[-1] } @list;
    check('inside_map_block', join(",", @r) eq "bar.txt,y.dat");
}
# inside a foreach/for-list loop
{
    my @out;
    for ("x/y", "p/q/r") {
        push @out, join("-", split m{/});
    }
    check('inside_foreach', join("|", @out) eq "x-y|p-q-r");
}
# explicit $_ argument still works (regression guard)
{
    local $_ = "a/b";
    my @r = split m{/}, $_;
    check('explicit_dollar_underscore_unaffected', join(",", @r) eq "a,b");
}
# explicit non-$_ argument still works (regression guard)
{
    my @r = split m{/}, "p/q/r";
    check('explicit_other_string_unaffected', join(",", @r) eq "p,q,r");
}
# scalar context: count of fields, still from $_
{
    local $_ = "a b c d";
    my $n = split ' ';
    check('scalar_context_implicit', $n == 4);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "split_implicit_underscore_done\n";
