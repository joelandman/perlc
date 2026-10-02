# Deep test for D186: a narrow, silent-wrong-condition bug in
# tryEmitI1Cond()'s file-scope-global comparison fast path
# (tryFileGlobalI64, Stage 26a). That fast path indexed
# fileScalarGlobals_ directly by bare variable name to let a
# `my VAR CMP literal` loop condition skip full PerlValue boxing --
# but it did this WITHOUT first checking whether a nearer lexical
# scope (an inner `my` with the same bare name) shadows the file-scope
# global, the way the general lookupVar()/lookupIntVar()/
# lookupFloatVar() resolution already correctly does everywhere else.
# So a C-style `for (my $i = 0; $i < N; $i++)` loop whose own `$i`
# happens to share a bare name with an EARLIER, unrelated file-scope
# `my $i` got its condition evaluated against the stale OUTER value
# instead of its own freshly-initialized one -- with no crash, no
# error, just the loop silently running zero iterations (or the wrong
# number) whenever the stale outer value already failed the
# condition. Minimal repro found via this project's own D184 deep
# test: a `my ($i,$j) = (...)` C-style loop, then a plain `my $i = 0;`
# at file scope, then ANOTHER `for (my $i = 0; ...)` whose condition
# silently read the second declaration's `$i` instead of its own.

# the original minimal repro, exactly as found
for (my ($i, $j) = (0, 10); $i < 3; $i++, $j--) {
    print "multi=$i,$j\n";
}
my $i = 0;
foreach (; $i < 2; $i++) {
    print "noinit=$i\n";
}
for (my $i = 0; $i < 2; $i++) {
    print "for_cstyle=$i\n";
}

# simplest possible shadowing shape: just two declarations
my $x = 5;
for (my $x = 0; $x < 2; $x++) {
    print "shadow=$x\n";
}

# shadowing where the OUTER value would make the condition false
# immediately if wrongly used (the exact failure signature of this bug)
my $y = 100;
for (my $y = 0; $y < 3; $y++) {
    print "shadow_false_trap=$y\n";
}

# shadowing inside a sub (free var resolving to an outer file-scope
# global is a DIFFERENT, legitimate case -- must still work -- but a
# sub-local `my` with the same name as a file-scope global must still
# correctly shadow it)
my $limit = 1;
sub inner_shadow {
    for (my $limit = 0; $limit < 4; $limit++) {
        print "sub_shadow=$limit\n";
    }
}
inner_shadow();

# regression: ordinary (non-shadowed) file-scope-global-in-condition
# fast path stays correct and fast
my $n = 3;
for (my $i2 = 0; $i2 < $n; $i2++) {
    print "plain=$i2\n";
}

print "done\n";
