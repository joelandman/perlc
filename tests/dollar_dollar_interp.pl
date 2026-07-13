#!/usr/bin/perl
# In-depth test suite for D59: `$$` (the PID special variable) never
# interpolated correctly inside a double-quoted string.
#
# Root cause: `parseStringInterp`'s raw-text scanner (parser.cpp) had no
# case at all for `$` immediately followed by a second `$`. None of its
# existing single-/two-character special-variable checks (`$@`, `$.`,
# `$,`, `$!`, `$/`, `$&`, `$0`, `$1`-`$9`, `${...}`, `$name`) match a `$`
# followed by another `$`, so the scanner fell through to its final,
# generic fallback — which just appends the *first* `$` as one literal
# character and moves on by a single position, leaving the *second* `$`
# to be rescanned from scratch on the next loop iteration. That second
# `$` then matched whatever *other* rule its own following character
# happened to satisfy: `"pid=$$here"` had its second `$` + `here` match
# the ordinary `$name` rule (interpolating the unrelated, undef `$here`
# variable) instead of being recognized as part of `$$`; `"file_$$.pl"`
# had its second `$` + `.` match the `$.` (input-line-number) special
# variable rule, garbling the whole thing into `"file_$0pl"`. `$$` used
# as a *bare* expression (`my $p = $$;`) was never affected — that path
# goes through the ordinary token-level parser (parsePrimary), which
# already had correct, dedicated `$$` handling.
#
# Fixed by giving `parseStringInterp` its own dedicated `$$` case,
# mirroring the token-level parser's exact rule: `$$` followed by a
# letter or underscore is a scalar dereference (`${$word}`, i.e.
# `NK::DerefScalar`); `$$` followed by anything else — including the end
# of the string — is the PID (`NK::GetpidFunc`), matching real Perl.
#
# NOT fixed here (out of scope, a separate and deeper pre-existing gap):
# a *subscripted* dereference immediately after `$$` inside a string
# (`"$$ref[0]"`/`"$$ref{k}"`, real Perl's shorthand for
# `"$ref->[0]"`/`"$ref->{k}"`) isn't specially handled — confirmed this
# is not new: even the equivalent *bare* expression (`$$ref[0]`, outside
# any string) already silently produces the wrong result under perlc
# today, independent of string interpolation entirely.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $pid = $$;

# ── Section 1: bare $$ inside a string, alone ──────────────────────────────
{
    my $s = "$$";
    check('dollar_dollar_alone', $s eq $pid);
}

# ── Section 2: $$ followed by whitespace, then more literal text ──────────
{
    my $s = "pid=$$ end";
    check('dollar_dollar_before_space', $s eq "pid=$pid end");
}

# ── Section 3: $$ immediately followed by punctuation (the original ───────
# ── "file_$$.pl" repro — previously garbled via an accidental $. match) ───
{
    my $s = "file_$$.pl";
    check('dollar_dollar_before_punct', $s eq "file_${pid}.pl");
}

# ── Section 4: $$ at the very end of the string (no trailing character ────
# ── at all to disambiguate against) ─────────────────────────────────────────
{
    my $s = "pid=$$";
    check('dollar_dollar_at_end', $s eq "pid=$pid");
}

# ── Section 5: $$word — dereference of a scalar ref, not the PID, when ────
# ── $$ is immediately followed by an identifier character ──────────────────
{
    my $x = 42;
    my $ref = \$x;
    my $s = "value: $$ref";
    check('dollar_dollar_word_is_deref', $s eq "value: 42");
}

# ── Section 6: $$ combined with a genuine variable interpolation in the ───
# ── same string ──────────────────────────────────────────────────────────────
{
    my $label = "worker";
    my $s = "$label-$$";
    check('dollar_dollar_with_other_interp', $s eq "$label-$pid");
}

# ── Section 7: multiple $$ occurrences in one string ───────────────────────
{
    my $s = "[$$][$$]";
    check('dollar_dollar_multiple', $s eq "[$pid][$pid]");
}

# ── Section 8: regression — plain variable interpolation elsewhere in the ─
# ── same file is completely unaffected ─────────────────────────────────────
{
    my $name = "World";
    my $s = "Hello, $name!";
    check('plain_interpolation_regression', $s eq "Hello, World!");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "dollar_dollar_interp_tests_done\n";
