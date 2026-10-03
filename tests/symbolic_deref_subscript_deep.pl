# Deep test for D188: ${EXPR}{key} / ${EXPR}[idx] -- the explicit-
# brace spelling of D63's $$name{key}/$$name[idx] -- silently produced
# WRONG data (not a parse error): NK::SymbolicDeref (built for the
# `${ EXPR }` form) had no case in the subscript-chain parser at all,
# so a following {key}/[idx] was never attached as a subscript. Worse,
# it wasn't even a parse error: the dropped subscript got silently
# re-parsed as an unrelated bare-block STATEMENT immediately
# following the real one (its value discarded), while the actual
# ${EXPR} alone (never dereferencing past the outer ref, and in the
# hashref/arrayref case wrongly routed through perl_deref_scalar,
# which only knows SCALAR refs) silently evaluated to undef. Found via
# a real Mail::Message-using script's `${$mail}{'From'}`.

my $mail = { From => 'a@b.com', To => 'c@d.com' };
my $aref = [10, 20, 30];

# the exact real-world repro, both as a plain assignment and directly
# as a function-call argument (the shape that originally surfaced as
# a hard parse error rather than silent wrong data)
my $x = ${$mail}{'From'};
print "assign=$x\n";
print "directarg=", ${$mail}{'To'}, "\n";

# the array-ref form
my $a1 = ${$aref}[1];
print "arrayform=$a1\n";

# chained: a deref-subscript immediately followed by another subscript
my $nested = { list => [100, 200] };
print "chained=", ${$nested}{'list'}[0], "\n";

# assignment TO a ${EXPR}{key} target (lvalue form)
${$mail}{'From'} = 'new@new.com';
print "lvalue=$mail->{From}\n";

# regression: plain ${EXPR} with no subscript (true scalar deref)
# stays correct
my $sref = \42;
print "scalarderef=", ${$sref}, "\n";

# regression: symbolic-by-name (string) scalar deref with no
# subscript stays correct
our $globalvar = "hi";
my $gname = "globalvar";
print "symbolic=", ${$gname}, "\n";

# regression: the sigil-chain form $$href{key}/$$aref[idx] (D63)
# stays unaffected
my $href2 = { y => 7 };
print "sigilchain=", $$href2{y}, "\n";
my $aref2 = [1, 2, 3];
print "sigilchain2=", $$aref2[2], "\n";

print "done\n";
