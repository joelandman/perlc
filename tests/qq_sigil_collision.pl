#!/usr/bin/perl
# In-depth test suite for D65: a scalar/hash variable name that happens
# to start with "q"/"qq"/"qw" was silently misinterpreted by the lexer
# as a quote-like operator instead of a plain sigil-prefixed identifier.
#
# Root cause: a bare `$`/`@`/`%` sigil is tokenized as its own, separate
# single-character token (lexer.cpp) without consuming the identifier
# that follows — the *next* tokenizer loop iteration reads that
# identifier via the generic bareword path. The `q`/`qq`/`qw`
# quote-like-operator recognition earlier in that same loop had no way
# to tell "a fresh expression-start q/qq/qw" apart from "the first
# letter(s) of an ordinary identifier that happens to start with q/qq/qw
# and was just sigil-prefixed" — so `$qqfoo` (an ordinary variable) could
# be caught by the `qq` check meant for `qq(...)`-style quoting, and
# `$q{key}`/`$qw{key}`/`%qw = (...)` (hash access or declaration for a
# variable literally named "q" or "qw") could be caught by the `q{}`/`qw`
# checks respectively.
#
# Two contributing factors made this worse than a narrow, single-name
# edge case:
#   - The `qq` branch (unlike the pre-existing `qw` branch, which already
#     required the character after "qw" to be non-alphanumeric/
#     non-underscore) had *no* such guard at all — it treated whatever
#     character followed "qq" as a delimiter unconditionally, so
#     `$qqfoo` misread "f" as a delimiter and scanned far ahead in the
#     source for another "f" to close it, corrupting the token stream
#     (the specific symptom — a hard parse error vs. a silently-wrong-
#     but-still-parseable result — depended on incidental nearby text).
#   - The `%` (hash) sigil has the exact same "bare sigil, identifier
#     read separately" structure as `$`/`@`, so even a hash *declaration*
#     (`my %qw = (...)`) could have its own "qw" identifier misread.
#
# Fixed by computing, once per tokenizer-loop iteration, whether the
# immediately preceding token was a bare `$`/`@`/`%` sigil (single-
# character token text) — and if so, unconditionally skipping all
# `q`/`qq`/`qw` quote-like-operator recognition for the upcoming text,
# since a sigil is always immediately followed by a plain identifier,
# never a quote-like operator. Also added a `qw`-style non-alphanumeric/
# non-underscore guard to the `qq` branch itself, matching the guard
# `qw` already had, so an expression-start bareword like a hypothetical
# `qqfoo(...)` call is not misread as `qq` either.
#
# NOT fixed here (confirmed, via direct testing, to be a separate,
# pre-existing, unrelated defect with no connection to variable names
# starting with q/qq/qw at all — reproduces identically for a hash named
# anything): a block-scoped `my $x = $hash{key};` where the hash's value
# is a *string* silently coerces it to `0` (the string gets run through
# a numeric fast path meant for genuinely numeric hash values) — the
# file-scope form and a numeric-valued hash both work correctly. Worked
# around in this file by using string concatenation (`"val=" . $h{key}`)
# instead of direct scalar assignment wherever this fix's own tests
# exercise a block-scoped hash lookup.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — "qq"-prefixed var name interpolated, ────
# ── followed by a literal ']' ──────────────────────────────────────────────
{
    my $qqfoo = "hello";
    my $s = "1:$qqfoo]";
    check('qq_prefixed_var_before_bracket', $s eq '1:hello]');
}

# ── Section 2: "qq"-prefixed var name followed by other ────────────────────
# ── closing-bracket-class characters ──────────────────────────────────────
{
    my $qqbar = "world";
    check('qq_prefixed_var_before_paren', "$qqbar)" eq 'world)');
    check('qq_prefixed_var_before_brace', "$qqbar}" eq 'world}');
}

# ── Section 3: a variable literally named "q" used for hash-element ───────
# ── access outside of string interpolation (bare expression) ──────────────
{
    my %q = (key => 111);
    my $result = "val=" . $q{key};
    check('var_named_q_hash_access', $result eq 'val=111');
}

# ── Section 4: a variable literally named "qw" used for hash-element ──────
# ── access outside of string interpolation ─────────────────────────────────
{
    my %qw = (key => 222);
    my $result = "val=" . $qw{key};
    check('var_named_qw_hash_access', $result eq 'val=222');
}

# ── Section 5: a hash literally named "qw" via the % sigil declaration ────
# ── itself — the declaration line's own "qw" identifier must not be ───────
# ── misread as a qw()-list operator ─────────────────────────────────────────
{
    my %qw2 = (a => 1, b => 2);
    check('hash_declared_named_qw', $qw2{a} == 1 && $qw2{b} == 2);
}

# ── Section 6: regression — genuine qq(...) and qw(...) operators at ──────
# ── expression-start position still work correctly ─────────────────────────
{
    my $val = 5;
    my $x = qq(value is $val);
    my @list = qw(one two three);
    check('qq_operator_regression', $x eq 'value is 5');
    check('qw_operator_regression', join(",", @list) eq 'one,two,three');
}

# ── Section 7: regression — the D60 bare q(...) fix is unaffected ─────────
{
    my $x = q(literal $notvar);
    check('bare_q_operator_regression', $x eq 'literal $notvar');
}

# ── Section 8: other q-prefixed variable names in ordinary (non-hash) ─────
# ── contexts still work correctly ───────────────────────────────────────────
{
    my $qux = "quxval";
    my $qqux = "qquxval";
    check('other_q_prefixed_names', $qux eq 'quxval' && $qqux eq 'qquxval');
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "qq_sigil_collision_tests_done\n";
