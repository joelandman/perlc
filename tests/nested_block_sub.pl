#!/usr/bin/perl
# In-depth test suite for named subs declared inside bare `{ }` blocks and
# other nested constructs (if/while/for/eval bodies).
#
# Root cause (D45): CodeGen::compile() collected sub definitions for
# forward-declaration/emission (subs_) by scanning only program.args'
# direct top-level children for NK::SubDef — completely missing any named
# sub nested inside a bare `{ }` block, if/elsif/else branch, while/for
# body, or eval block, at any depth. Real Perl always compile-time-hoists
# a *named* sub declaration to package scope, regardless of block
# nesting (unlike `my` variables, which really are block-scoped) — even
# a sub inside a never-executed `if (0) { ... }` branch is still
# globally callable. Since perlc's emitStmt's `case NK::SubDef:` is
# already a documented no-op ("bodies already emitted in compile()"),
# any sub missing from subs_ never got its LLVM function body emitted at
# all; calling it silently fell through to a not-found fallback and
# returned an empty/undef value without ever running the sub's body.
#
# Fixed by replacing the direct-children-only scan with a recursive
# collectSubDefs() that walks every child field a Node can hold (left,
# right, cond, body, init, step, args, branches) — mirroring the existing
# hasWantarrayOrUserCall() traversal — so subs_ now contains every named
# sub in the program regardless of nesting depth.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — sub declared and called in the same ─────
# ── single bare block, no preceding blocks at all ───────────────────────────
{
    sub add_one { my ($x) = @_; return $x + 1; }
    check('sub_in_single_block', add_one(4) == 5);
}

# ── Section 2: a preceding sibling block (the originally-reported repro ───
# ── shape), then the sub-containing block ───────────────────────────────────
{
    my $unused = 1;
}
{
    sub double_it { my ($x) = @_; return $x * 2; }
    check('sub_after_one_sibling_block', double_it(5) == 10);
}

# ── Section 3: two preceding sibling blocks ────────────────────────────────
{
    my $a = 1;
}
{
    my $b = 2;
}
{
    sub triple_it { my ($x) = @_; return $x * 3; }
    check('sub_after_two_sibling_blocks', triple_it(5) == 15);
}

# ── Section 4: sub declared inside a block, called from OUTSIDE after the ─
# ── block closes ────────────────────────────────────────────────────────────
{
    sub quad_it { my ($x) = @_; return $x * 4; }
}
check('sub_called_after_block_closes', quad_it(5) == 20);

# ── Section 5: sub nested two blocks deep ───────────────────────────────────
{
    {
        sub nested_two_deep { return "deep"; }
    }
}
check('sub_nested_two_blocks_deep', nested_two_deep() eq "deep");

# ── Section 6: sub declared inside an if-branch that IS taken ──────────────
{
    if (1) {
        sub in_if_branch { return "if-taken"; }
    }
    check('sub_in_taken_if_branch', in_if_branch() eq "if-taken");
}

# ── Section 7: sub declared inside an if-branch that is NEVER taken — ──────
# ── still globally callable, matching real Perl's compile-time hoisting ────
{
    if (0) {
        sub never_taken_branch { return "shouldnt matter"; }
    }
    check('sub_in_untaken_if_branch', never_taken_branch() eq "shouldnt matter");
}

# ── Section 8: sub declared inside a for-loop body ──────────────────────────
{
    for (1..1) {
        sub in_for_loop { my ($x) = @_; return $x * 10; }
    }
    check('sub_in_for_loop_body', in_for_loop(3) == 30);
}

# ── Section 9: sub declared inside a while-loop body ────────────────────────
{
    my $i = 0;
    while ($i < 1) {
        sub in_while_loop { return "while-body"; }
        $i++;
    }
    check('sub_in_while_loop_body', in_while_loop() eq "while-body");
}

# ── Section 10: sub declared inside an eval{} block ─────────────────────────
{
    eval {
        sub in_eval_block { return "eval-body"; }
    };
    check('sub_in_eval_block', in_eval_block() eq "eval-body");
}

# ── Section 11: recursive sub declared inside a bare block ──────────────────
# Deliberately NOT written as `my ($n)=@_; return EXPR-containing-self-call`
# (a 2-statement body) — that exact shape matches the AST-level inliner's
# "inline this sub at its call site" pattern, and a *directly self-
# recursive* sub matching it hits an unrelated, pre-existing compiler
# crash (infinite compile-time inlining, logged separately as D57, not
# fixed here). The 3-statement body below doesn't match the inliner's
# shape, so it compiles as a normal (non-inlined) recursive call.
{
    sub fact_in_block {
        my ($n) = @_;
        my $result = $n <= 1 ? 1 : $n * fact_in_block($n - 1);
        return $result;
    }
    check('recursive_sub_in_block', fact_in_block(5) == 120);
}

# ── Section 12: regression — anonymous subs / closures inside blocks are ──
# ── unaffected (still real closures, not hoisted globals) ──────────────────
{
    my $mult = 7;
    my $closure = sub { my ($x) = @_; return $x * $mult; };
    check('closure_in_block_regression', $closure->(6) == 42);
}

# ── Section 13: regression — file-scope subs (no block at all) still work ──
sub file_scope_sub { return "file-scope"; }
check('file_scope_sub_regression', file_scope_sub() eq "file-scope");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "nested_block_sub_tests_done\n";
