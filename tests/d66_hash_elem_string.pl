#!/usr/bin/perl
# In-depth test suite for D66: a block-scoped `my $x = $hash{key};` silently
# coerced the value to 0 whenever the hash's stored value was a string (or
# any other non-numeric type), while file-scope declarations and numeric
# hash values were unaffected.
#
# Root cause: CodeGen::canEmitF64's NK::HashElem case (codegen.cpp) returned
# true whenever the *hash variable itself* was in scope, with no check at
# all on whether the *value actually stored at that key* was numeric. The
# matching CodeGen::emitExprF64 case then unconditionally called
# perl_to_float() on whatever perl_hash_get_str_ref returned, which silently
# produces 0.0 for a non-numeric string. NK::My's numeric-fast-path codegen
# (also implicated in D64) is gated behind `!atFileScope`, which is why only
# block-scoped declarations were affected — file-scope assignments never
# reach canEmitF64 for this pattern at all, not because the underlying
# HashElem check was actually safe.
#
# Fixed by making both canEmitF64's and emitExprF64's NK::HashElem cases
# conservative: canEmitF64 now always returns false for a hash element (the
# same treatment ArrayElem and the hash-ArrowDeref case already received),
# and emitExprF64 returns nullptr (bail to the general, tag-checking path)
# instead of blindly calling perl_to_float. This sacrifices the unboxed-
# double fast path for hash element reads in exchange for correctness;
# there is no per-hash value-type tracking analogous to arrayElemTypes_ to
# safely re-enable it selectively.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original repro — block-scoped string value ──────────────
{
    my %h = (key => "strval");
    my $x = $h{key};
    check('block_string_value', $x eq "strval");
}

# ── Section 2: numeric values still work (int) ──────────────────────────────
{
    my %h = (key => 42);
    my $x = $h{key};
    check('block_int_value', $x == 42);
    my $y = $x + 1;
    check('block_int_value_arith', $y == 43);
}

# ── Section 3: numeric values still work (float) ────────────────────────────
{
    my %h = (key => 3.14);
    my $x = $h{key};
    check('block_float_value', abs($x - 3.14) < 1e-9);
    my $sq = $x * $x;
    check('block_float_value_arith', abs($sq - 9.8596) < 1e-6);
}

# ── Section 4: string value used in string context after assignment ───────
{
    my %h = (name => "Alice");
    my $n = $h{name};
    my $greeting = "Hello, " . $n . "!";
    check('block_string_concat', $greeting eq "Hello, Alice!");
}

# ── Section 5: string value used in interpolation ──────────────────────────
{
    my %h = (name => "Bob");
    my $n = $h{name};
    my $s = "Hi $n";
    check('block_string_interp', $s eq "Hi Bob");
}

# ── Section 6: not name-specific — different hash variable names ──────────
{
    my %other = (k => "value_other");
    my $x = $other{k};
    check('block_hash_named_other', $x eq "value_other");
}
{
    my %data = (k => "value_data");
    my $x = $data{k};
    check('block_hash_named_data', $x eq "value_data");
}

# ── Section 7: nested blocks (2+ levels deep) ───────────────────────────────
{
    {
        {
            my %h = (deep => "nested_string");
            my $x = $h{deep};
            check('triple_nested_block_string', $x eq "nested_string");
        }
    }
}

# ── Section 8: inside a sub body ────────────────────────────────────────────
sub get_str_from_hash {
    my %h = (k => "sub_string");
    my $x = $h{k};
    return $x;
}
check('inside_sub_string', get_str_from_hash() eq "sub_string");

# ── Section 9: inside if/while/for blocks ───────────────────────────────────
{
    my %h = (k => "if_string");
    if (1) {
        my $x = $h{k};
        check('inside_if_block_string', $x eq "if_string");
    }
}
{
    my %h = (k => "while_string");
    my $i = 0;
    while ($i < 1) {
        my $x = $h{k};
        check('inside_while_block_string', $x eq "while_string");
        $i++;
    }
}
{
    my %h = (k => "for_string");
    for (my $i = 0; $i < 1; $i++) {
        my $x = $h{k};
        check('inside_for_block_string', $x eq "for_string");
    }
}

# ── Section 10: mixed hash with both string and numeric values ─────────────
{
    my %h = (s => "mixed_string", n => 99);
    my $sv = $h{s};
    my $nv = $h{n};
    check('mixed_hash_string_field', $sv eq "mixed_string");
    check('mixed_hash_numeric_field', $nv == 99);
}

# ── Section 11: undef value ─────────────────────────────────────────────────
{
    my %h = (k => undef);
    my $x = $h{k};
    check('block_undef_value', !defined($x));
}

# ── Section 12: hash-ref and array-ref values (not just plain strings) ─────
# NOTE: uses a mixed-type array ([1,"two",3]) rather than an all-numeric one
# deliberately: an all-numeric literal like [1,2,3] hits an unrelated,
# separate, pre-existing bug (found while writing this test, logged as
# TESTS.md D83 — NOT fixed here) where ref() fails to recognize the
# FLAT_ARRAY-tagged optimization path as an ARRAY ref. Confirmed that bug
# reproduces even with no hash involved at all (plain `my $y=[4,5,6];
# ref($y)`), so it is unrelated to D66's hash-element fix.
{
    my %h = (aref => [1,"two",3]);
    my $x = $h{aref};
    check('block_arrayref_value', ref($x) eq 'ARRAY' && $x->[1] eq 'two');
}
{
    my %h = (href => {a => 1});
    my $x = $h{href};
    check('block_hashref_value', ref($x) eq 'HASH' && $x->{a} == 1);
}

# ── Section 13: numeric-looking string stays a string (not silently 0'd,
#    and not silently coerced to the numeric value either) ─────────────────
{
    my %h = (k => "007");
    my $x = $h{k};
    check('block_numeric_looking_string', $x eq "007");
}

# ── Section 14: file-scope regression (must remain correct, unaffected by
#    this fix since it never used the buggy fast path in the first place) ──
our %file_scope_h = (key => "file_scope_string");
our $file_scope_x = $file_scope_h{key};
check('file_scope_string_value', $file_scope_x eq "file_scope_string");

# ── Section 15: reassignment after initial read ─────────────────────────────
{
    my %h = (k => "first");
    my $x = $h{k};
    $h{k} = "second";
    my $y = $h{k};
    check('reassignment_first_read', $x eq "first");
    check('reassignment_second_read', $y eq "second");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d66_hash_elem_string_done\n";
