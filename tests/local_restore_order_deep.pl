# Bare-block local-restore order: perl_local_restore_to() must run
# before popScope(). A real heap-corruption SEGFAULT, found while
# writing the multi-key hash subscript regression test.
# `local $ref->{key} = val;` where $ref is a lexical (`my`) variable
# of the SAME block/sub that `local` runs in sets up a save/restore
# target that's a raw PerlValue* pointing INTO that ref's underlying
# container (a hash cell) -- not an independently-owned value. Three
# separate call sites (emitBlock, emitBlockLast, and the top-level
# program body) each called popScope() -- which frees `my` variables,
# including $ref, dropping its hash's refcount and freeing its cells
# -- BEFORE calling perl_local_restore_to(), which then wrote the
# saved old value through what was now a DANGLING pointer into freed
# memory. This silently corrupts the allocator's freelist: no crash
# at the point of corruption, no error, no wrong-looking output from
# the local() itself -- it only surfaces as a SEGFAULT on some LATER,
# completely unrelated PerlValue allocation (here, a hash slice
# evaluated afterward) once that corrupted freelist is walked again.
# Every other local-restore call site (sub exit, loop bodies, early
# `return`/`goto &sub`) already had the order right; these three did
# not. Found via a real-world-shaped combination: a sub using
# `local $ref->{key}`, called, followed by ordinary further code.

# the exact original repro: inside a sub
sub f {
    my $r = { z => 1 };
    local $r->{z} = 5;
    print "sub_local=$r->{z}\n";
}
f();

# the same pattern directly in a bare block (not inside any sub) --
# found to need a SEPARATE fix (emitBlock, not just emitBlockLast)
{
    my $r2 = { z => 10 };
    local $r2->{z} = 50;
    print "block_local=$r2->{z}\n";
}

# the same pattern directly at file/top-level scope (found to need a
# THIRD separate fix, in the main-program-body emitter)
our $r3 = { z => 100 };
local $r3->{z} = 500;
print "toplevel_local=$r3->{z}\n";

# the actual crash trigger: ANY further PerlValue allocation after the
# corrupting local() -- a hash slice is what the original real-world
# shape used, but the point is this must not crash no matter what
# runs afterward
my %s = (a => 1, b => 2, c => 3);
my @vals = @s{qw(a b)};
print "after_slice=@vals\n";

# more allocations for good measure, since freelist corruption can
# manifest unpredictably several allocations later
my @acc;
for my $i (1..20) {
    my $tmp = { n => $i };
    push @acc, $tmp->{n};
}
print "after_loop=ok\n";

print "done\n";
