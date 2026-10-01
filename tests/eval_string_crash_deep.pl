# Deep test for D167: a batch of findings from a real /usr/bin/json_pp
# script (`'eval' => sub { my $v = eval "no strict;\n#line 1
# \"input\"\n$_"; die "$@" if $@; return $v; }`).
#
# 1. A genuine COMPILER CRASH: inlining a constant eval STRING whose
#    body hits a codegen-level compile-time error (here: a parenless
#    bareword call to an unresolved name) threw a C++ exception while
#    LLVM basic blocks for the eval's setjmp/longjmp machinery were
#    mid-construction, escaping past the code that would normally
#    terminate them -- a hard "Basic Block ... does not have a
#    terminator!" LLVM verify-error crash, not a graceful `eval`
#    catching a compile error the way real Perl does. Fixed by
#    catching the exception where resultAlloca/endBB are in scope
#    (case NK::EvalBlock) and finishing the eval exactly like a caught
#    runtime `die` would, with $@ set to the error text.
# 2. Found while fixing (1): perlc unconditionally treated ANY
#    parenless bareword call to an unresolved name as a hard compile
#    error, but real Perl only does that when the bareword has an
#    ARGUMENT (`foo "arg";`) -- a bare, argument-less name (`foo;`
#    alone) silently auto-quotes to the string "foo" instead when it
#    never resolves to a sub (verified: `my $x = bar; print "$x\n";`
#    with no `use strict` prints "bar", no error). perlc doesn't track
#    `use strict 'subs'` scoping anywhere (nothing in this codebase
#    does), so it always takes the more-permissive no-strict real-Perl
#    behavior -- this is why the eval STRING below includes an
#    explicit leading "no strict;", exactly matching real json_pp's
#    own eval string (`"no strict;\n#line 1 \"input\"\n$_"`): real
#    Perl's `eval STRING` inherits the calling scope's `use strict`
#    (verified separately), so without that explicit "no strict;" the
#    two engines would disagree whenever the *outer* file happens to
#    have `use strict` in effect -- not what this fix is about. This
#    file deliberately has no file-scope `use strict;` for the same
#    reason.
use warnings;

# (1): used to be a hard LLVM verify-error crash at compile time
my $v1 = eval 'no strict; no warnings; bar';
print "err1_has_text=", (length($@) > 0 ? "yes" : "no"), "\n";
print "v1_defined=", (defined($v1) ? "yes" : "no"), "\n";

# a successful eval afterward still works (the eval machinery itself
# -- $@ reset, setjmp/longjmp stack -- wasn't left corrupted)
my $v2 = eval '2 + 2';
print "err2_empty=", ($@ eq "" ? "yes" : "no"), "\n";
print "v2=$v2\n";

# a genuine runtime die inside an eval STRING still works (the fix
# must not affect the normal die/longjmp path at all)
my $v3 = eval 'die "boom\n"; 1';
print "err3=$@";
print "v3_defined=", (defined($v3) ? "yes" : "no"), "\n";

# (2): a bare, argument-less, unresolved bareword auto-quotes outside
# eval too, not just inside one
{
    no strict 'subs';
    no warnings 'syntax';
    my $x = zork;
    print "x=$x\n";
}

# (2) regression: a bareword WITH an argument, unresolved, is still a
# hard compile error (verified separately, not run here since it must
# NOT compile) -- see tests/eval_string_crash_deep's sibling note: a
# standalone `foo "arg";` script fails to compile with both real Perl
# and perlc, by design, so no runtime check is needed here.

# (2) regression: a bareword that DOES resolve to a real sub is
# unaffected -- still calls it, not autoquoted
sub known_sub { "called" }
print "known=", known_sub(), "\n";
my $y = known_sub;
print "y=$y\n";

print "done\n";
