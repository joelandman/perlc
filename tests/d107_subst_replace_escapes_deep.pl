#!/usr/bin/perl
# Deep: D107 — s/PATTERN/REPLACEMENT/'s REPLACEMENT text is parsed like a
# double-quoted string in real Perl (backslash escapes processed, plus
# $1../$&  capture interpolation), but perlc's replacement-expansion loop
# (src/runtime.c perl_regex_subst) only ever handled $0-$9/$& and copied
# every other character — including backslash-escape sequences — straight
# through verbatim. Found via a real script (/usr/bin/debconf-escape's
# `s/\\/\\\\/g; s/\n/\\n/g;`), which doubled the inserted backslash.
#
# Fix: perl_regex_subst's expansion loop now recognizes \\ \n \t \r \f \b
# \a \e \0 \$ \@ before falling back to "unknown escape: drop the
# backslash, keep the character" for anything else — matching Perl's own
# fallback behavior for an unrecognized double-quote escape.
#
# NOT fixed here (a separate, much larger gap — logged, not part of
# D107): arbitrary variable interpolation ($name, @arr) in replacement
# text doesn't work at all — only $0-$9/$& (capture refs) are
# recognized. That requires codegen-level string-interpolation support
# for the replacement text, not a runtime escape-table fix.
use strict;
use warnings;
no warnings 'misc'; # real perl warns on the deliberate unknown-escape test below

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

{
    # the exact real-world repro
    my $s = "hello world\n";
    $s =~ s/\\/\\\\/g;
    $s =~ s/\n/\\n/g;
    check('debconf_escape_repro', $s eq "hello world\\n");
}
{
    my $s = "X";
    $s =~ s/X/\\/;
    check('single_escaped_backslash', $s eq "\\" && length($s) == 1);
}
{
    my $s = "X";
    $s =~ s/X/\t/;
    check('tab_escape', $s eq "\t" && length($s) == 1);
}
{
    my $s = "X";
    $s =~ s/X/a\nb/;
    check('newline_escape', $s eq "a\nb" && length($s) == 3);
}
{
    # \f \a \e compared via ord() rather than embedding them in a plain
    # "..." string literal for comparison — plain double-quoted string
    # literals don't recognize \f/\a/\e/\b at all (a separate,
    # pre-existing lexer gap unrelated to s/// replacement text; see
    # TESTS.md D108), which would otherwise make this check's own
    # "expected" value wrong instead of testing the s/// fix.
    my $s = "X";
    $s =~ s/X/\r\f\a\e/;
    my @ords = map { ord($_) } split //, $s;
    check('other_escapes', "@ords" eq "13 12 7 27");
}
{
    # unknown escape: backslash dropped, character kept literally
    my $s = "X";
    $s =~ s/X/\z/;
    check('unknown_escape_fallback', $s eq "z");
}
{
    # escaped $ must not trigger capture-group interpolation
    my $s = "X";
    $s =~ s/X/\$1 literally/;
    check('escaped_dollar', $s eq "\$1 literally");
}
{
    # capture groups still work alongside a literal backslash-n
    my $s = "abc123";
    $s =~ s/(\d+)/[$1]\n/;
    check('capture_with_escape', $s eq "abc[123]\n");
}
{
    # /g with escapes across multiple matches
    my $s = "a.b.c";
    $s =~ s/\./\t/g;
    check('global_with_escape', $s eq "a\tb\tc");
}
{
    # /e path (eval'd replacement code) must be unaffected — different
    # runtime function (perl_regex_subst_e), not touched by this fix.
    my $s = "5";
    $s =~ s/5/2+3/e;
    check('eval_replacement_unaffected', $s eq "5");
}
{
    # whole-match $& alongside an escape
    my $s = "abc";
    $s =~ s/b/[$&]\t/;
    check('ampersand_with_escape', $s eq "a[b]\tc");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d107_subst_replace_escapes_done\n";
