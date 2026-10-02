# Deep test for D179: `not` nested inside a tighter-binding
# expression, e.g. `$a && not $b`, was a hard "unexpected token
# 'not'" parse error. `not` was only ever reachable at the very
# bottom of the precedence chain (parseLowNot(), below even
# assignment) -- but real Perl's `not`, despite genuinely having
# lower precedence than `&&`, is also a prefix operator usable
# wherever a term is expected; when nothing follows for it to
# (loosely) absorb, `not $b` becomes a complete, self-contained
# operand, exactly as if written `$a && (not $b)`. Found via a real
# /usr/bin/podebconf-report-po script:
# `if ($LANGUAGETEAM_ARG && defined $CALL && not $CALL_WITH_TRANSLATORS)`.
my $a = 1;
my $b = 0;
my $c = 1;

print "and_not=", (($a && not $b) ? "y" : "n"), "\n";
print "or_not=", (($b || not $c) ? "y" : "n"), "\n";
print "not_leading=", ((not $b) && $a ? "y" : "n"), "\n";
print "chain=", (($a && $c && not $b) ? "y" : "n"), "\n";

# the exact real-world shape: three && operands, the last one a
# not-prefixed variable
my $team_arg = 1;
my $call = 1;
my $with_translators = 0;
if ($team_arg && $call && not $with_translators) {
    print "real_world_shape=triggered\n";
} else {
    print "real_world_shape=not_triggered\n";
}

# regression: `not => value` auto-quote (bareword before =>) must
# stay completely unaffected -- the new nested-not check must not
# intercept this
my %h = (not => 5, foo => 6);
print "autoquote=$h{not} $h{foo}\n";

# regression: plain statement-level `not EXPR` (the existing,
# already-working case) is unaffected
print "plain_not=", (not 0), "\n";
if (not $b) {
    print "if_not=triggered\n";
}

print "done\n";
