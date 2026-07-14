#!/usr/bin/perl
# In-depth test suite for D34: `defined EXPR` without surrounding parens
# was a hard parse error.
#
# Root cause: the parser's `defined` handling (parser.cpp) unconditionally
# called `consume(TK::LPAREN, "(")` right after the `defined` keyword —
# there was no optional-parens handling at all, unlike neighboring named
# unary operators (`ref`, `abs`, `int`, `sqrt`, `exists`, etc.), which
# already supported `match(TK::LPAREN)` to make the paren optional.
#
# Fixed by making the paren optional the same way those neighbors do, but
# with one deliberate difference: this parser's `ref`/`abs`/`int`/etc. all
# call the *full* `parseExpr()` for their bare (no-paren) argument
# regardless of whether parens were present, which doesn't reproduce real
# Perl's actual "named unary operator" precedence (tighter than
# comparisons/equality, looser than shift/additive/multiplicative) — it
# would over-consume a trailing `&&`/`||`/comparison as part of the
# argument instead of stopping before it. `defined` needed to get this
# right, since `if (defined $x) {...}`'s sibling idiom `return unless
# defined $x` and compound conditions like `if (defined $x && $y)` are
# both extremely common. Calling `parseShift()` instead of `parseExpr()`
# for the no-parens case reproduces the correct precedence exactly, since
# `parseShift` already sits at the right spot in this parser's own
# precedence chain (directly below `parseBinding`/`parseCmp`, directly
# above `parseAdd`/`parseMul`) — an explicit paren still parses the full
# expression inside, since parens always override precedence.
#
# Deliberately does NOT `use warnings`: Section 4 intentionally does
# arithmetic on an undef value ($undef + 1) to test operator precedence,
# which triggers real Perl's harmless "Use of uninitialized value" runtime
# warning — perlc doesn't emit `use warnings` diagnostics at all (D56) and
# has no `no warnings` support yet (D48), so the warning would otherwise
# break this file's exact-match harness comparison. Same precedent as
# other files in this suite (e.g. sort_scalar_context.pl for D29,
# d73_slice_interp.pl for D73).
use strict;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original repro — basic defined/undef without parens ────
{
    my $x = 5;
    check('basic_defined', defined $x);
}
{
    my $y;
    check('basic_undef', !defined $y);
}

# ── Section 2: the parenthesized form still works (regression) ─────────────
{
    my $x = 5;
    check('parenthesized_form', defined($x));
}

# ── Section 3: common idiomatic usage — if/unless/return-unless ───────────
{
    my $x = 5;
    my $ok = 0;
    if (defined $x) { $ok = 1; }
    check('if_defined', $ok);
}
{
    my $y;
    my $ok = 0;
    unless (defined $y) { $ok = 1; }
    check('unless_defined', $ok);
}
{
    sub check_defined_return {
        my ($v) = @_;
        return "no" unless defined $v;
        return "yes";
    }
    check('return_unless_defined_true', check_defined_return(5) eq "yes");
    check('return_unless_defined_false', check_defined_return(undef) eq "no");
}

# ── Section 4: precedence — defined binds tighter than &&/||, looser than
#    +/-/*/shift (real Perl's "named unary operator" precedence) ──────────
{
    my $x = 5;
    my $y = 0;
    # defined $x && $y  ==  (defined $x) && $y  ==  1 && 0  ==  0
    my $r = defined $x && $y;
    check('precedence_and', $r == 0);
}
{
    my $x = 5;
    my $y = 0;
    # defined $x || $y  ==  (defined $x) || $y  ==  1
    my $r = defined $x || $y;
    check('precedence_or', $r == 1);
}
{
    my $x = 5;
    # defined $x + 1  ==  defined($x + 1)  ==  defined(6)  ==  true
    # (this is still true either way since $x+1 is always defined when $x
    # is, but it confirms the arithmetic itself doesn't get chopped off)
    my $r = defined $x + 1;
    check('precedence_add_still_parses', $r == 1);
}
{
    my $x = 5;
    # defined $x < 100  ==  (defined $x) < 100  ==  1 < 100  ==  1
    my $r = defined $x < 100;
    check('precedence_relational', $r == 1);
}
{
    my $undef;
    # defined $undef + 1  ==  defined($undef + 1)  ==  defined(1)  ==  true
    # (undef numifies to 0, so $undef+1 is 1, which IS defined — this
    # specifically distinguishes "defined($undef+1)" from "(defined
    # $undef) + 1", since the former is 1 (true) and the latter would be
    # "" + 1 == 1 too, coincidentally — see the next section for a case
    # that actually distinguishes the two unambiguously)
    my $r = defined $undef + 1;
    check('precedence_add_with_undef', $r == 1);
}
# ── Section 5: defined on hash/array elements and dereferences ─────────────
{
    my %h = (a => 1);
    check('hash_elem_present', defined $h{a});
    check('hash_elem_missing', !defined $h{b});
}
{
    my @arr = (1, 2, 3);
    check('array_elem_present', defined $arr[0]);
    check('array_elem_missing', !defined $arr[10]);
}
{
    my $obj = { field => "val" };
    check('arrow_deref', defined $obj->{field});
    check('arrow_deref_missing', !defined $obj->{missing});
}

# ── Section 6: negation combined with no-parens defined ─────────────────────
{
    my $y;
    check('negated_defined', !defined $y);
    my $x = 5;
    check('negated_defined_false', !(!defined $x));
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d34_defined_noparens_done\n";
