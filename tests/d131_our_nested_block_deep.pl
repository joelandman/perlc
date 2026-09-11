#!/usr/bin/perl
# Deep test for D131: `our $var;` / `our @arr;` / `our %hash;` declared
# inside a nested bare `{ }` block. Two stacked bugs found and fixed:
#
# 1. The scalar declaration branch (codegen.cpp, case NK::My) gated its
#    global-vs-local storage decision on `atFileScope` alone, ignoring
#    `isOur` — so `our $x;` inside a nested block (isOur=true,
#    atFileScope=false) fell through to the local-alloca fast path
#    instead of getting real global storage, and a sub referencing the
#    same name from outside the block saw a disconnected variable.
#
# 2. Once that was fixed, a second, more general bug surfaced: EVERY
#    textual occurrence of a plain `our $x;`/`our @a;`/`our %h;` (not
#    just the first) unconditionally minted a *brand-new* global
#    (LLVM auto-renames the symbol to dodge the clash), instead of
#    reusing the one already registered under this package-qualified
#    name — so `our $x = 5;` at file scope plus `our $x;` inside a sub
#    (or a second nested block) produced two disconnected storage
#    locations. The array/hash `our` declaration branches already had
#    correct qualified-name-based reuse logic (D112); the plain scalar
#    branch and the `:shared` scalar branch did not. Fixed by looking
#    up the existing global by qualified name before creating a new
#    one, and — since a bare `our $x;`/`our @a;`/`our %h;` with no
#    initializer must leave the existing value untouched, matching
#    real Perl — only resetting storage to a fresh undef/empty value
#    when the global is newly created.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

{
    our $scalar_val;
    sub set_scalar { $scalar_val = shift; }
}
set_scalar("hello");
{
    our $scalar_val;
    check('scalar_nested_block', $scalar_val eq "hello");
}

{
    our @arr_val;
    sub set_arr { @arr_val = @_; }
}
set_arr(1, 2, 3);
{
    our @arr_val;
    check('array_nested_block', "@arr_val" eq "1 2 3");
}

{
    our %hash_val;
    sub set_hash { %hash_val = @_; }
}
set_hash(x => 1, y => 2);
{
    our %hash_val;
    check('hash_nested_block',
        join(",", map { "$_=$hash_val{$_}" } sort keys %hash_val) eq "x=1,y=2");
}

# `our $x;` (bare redeclaration, no initializer) must not reset an
# already-set value — a common idiom for bringing an existing package
# var into scope inside a sub.
{
    our $counter = 0;
}
sub bump { our $counter; $counter++; }
bump(); bump(); bump();
{
    our $counter;
    check('scalar_redeclared_shared', $counter == 3);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d131_our_nested_block_done\n";
