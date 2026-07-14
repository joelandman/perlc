#!/usr/bin/perl
# In-depth test suite for D72: arrow-dereference expressions did not
# interpolate inside double-quoted strings.
#
# Root cause: Parser::parseStringInterp's raw-character scanner for
# "$varname" recognized only a bare trailing "[idx]" or "{key}" (plain
# @varname/%varname element access) — it had no notion of "->" at all.
# "$href->{a}" scanned "$href" as a plain scalar (stringifying the ref,
# e.g. "HASH(0x...)"), then hit "-" with no matching rule, falling through
# to the default literal-character path — so the arrow and subscript were
# emitted as literal text glued onto the ref's stringification instead of
# being evaluated as a dereference.
#
# Fixed by extending the same scanner into a small loop after the variable
# name is read: it now recognizes an explicit "->" immediately followed by
# "[" or "{" as the start of a dereference chain (building ArrowDeref nodes,
# the same AST shape the token-based parser produces for non-interpolated
# code), and continues consuming further adjacent "[idx]"/"{key}" segments
# (with or without their own "->") as further chain links — mirroring real
# Perl's rule that $ref->{a}{b} and $ref->{a}->{b} are equivalent, and only
# the first subscript in a chain needs an explicit arrow. An explicit "->"
# NOT followed by "[" or "{" (e.g. "$obj->method") is deliberately left
# unconsumed, since real Perl's string interpolation doesn't evaluate
# method calls or code-ref calls either — only "->[" and "->{" chains.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original repros ──────────────────────────────────────────
{
    my $href = {a => 1};
    check('hash_arrow_basic', "elem: $href->{a}" eq "elem: 1");
}
{
    my $aref = [10, 20, 30];
    check('array_arrow_basic', "elem: $aref->[1]" eq "elem: 20");
}

# ── Section 2: chained subscripts, with and without repeated "->" ──────────
{
    my $ref = {a => {b => "deep"}};
    check('chain_no_repeat_arrow', "chain: $ref->{a}{b}" eq "chain: deep");
    check('chain_repeat_arrow',    "chain: $ref->{a}->{b}" eq "chain: deep");
}
{
    my $aoh = [{name => "alice"}, {name => "bob"}];
    check('array_of_hashrefs_no_arrow', "aoh: $aoh->[0]{name}" eq "aoh: alice");
    check('array_of_hashrefs_arrow',    "aoh: $aoh->[1]->{name}" eq "aoh: bob");
}
{
    my $ref = {a => {b => {c => "triple"}}};
    check('triple_nested_chain', "triple: $ref->{a}{b}{c}" eq "triple: triple");
}
{
    my $aoa = [[1, 2], [3, 4]];
    check('array_of_arrays_no_arrow', "aoa: $aoa->[1][0]" eq "aoa: 3");
}

# ── Section 3: variable index/key inside a chain ────────────────────────────
{
    my $aoh = [{name => "alice"}, {name => "bob"}];
    my $i = 1;
    check('var_index_in_chain', "varidx: $aoh->[$i]{name}" eq "varidx: bob");
}
{
    my $ref = {a => {b => "deep"}};
    my $k = "b";
    check('var_key_in_chain', "varkey: $ref->{a}{$k}" eq "varkey: deep");
}

# ── Section 4: negative array index ─────────────────────────────────────────
{
    my $aref = [10, 20, 30];
    check('negative_index', "neg: $aref->[-1]" eq "neg: 30");
}

# ── Section 5: at the very end of the string (no trailing text) ────────────
{
    my $aref = [1, 2, 3];
    check('end_of_string', "end: $aref->[2]" eq "end: 3");
}

# ── Section 6: an explicit "->" NOT followed by [ or { is left as literal
#    text (real Perl doesn't call methods/code-refs during interpolation) ──
{
    package Foo;
    sub new { return bless {}, shift; }
    package main;
    my $obj = Foo::new('Foo');
    my $s = "text $obj->foo more";
    # $obj stringifies as "Foo=HASH(0x...)"; "->foo" must remain literal.
    check('arrow_method_not_interpolated', $s =~ /^text Foo=HASH\(0x[0-9a-f]+\)->foo more$/);
}

# ── Section 7: regressions — plain (non-arrow) hash/array interpolation ────
{
    my %h = (k => "v");
    check('plain_hash_regression', "plain hash: $h{k}" eq "plain hash: v");
}
{
    my @a = (1, 2, 3);
    check('plain_array_regression', "plain array: $a[1]" eq "plain array: 2");
}
{
    my $plain = "hi";
    check('plain_scalar_regression', "plain: $plain" eq "plain: hi");
}

# ── Section 8: ${...} brace-interpolation form is unaffected ───────────────
{
    my $x = 5;
    check('brace_form_regression', "brace: ${x}" eq "brace: 5");
}

# ── Section 9: bareword vs variable hash keys inside a chain ───────────────
{
    my $ref = { literal_key => "lit" };
    check('bareword_key_in_chain', "bw: $ref->{literal_key}" eq "bw: lit");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d72_arrow_interp_done\n";
