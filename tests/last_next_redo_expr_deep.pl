# Deep test for D172: `last`/`next`/`redo` used as an EXPRESSION —
# most commonly as the trailing operand of a comma expression
# immediately followed by an if/unless modifier
# (`$x = EXPR, last if COND;`) — was a hard "unexpected token
# 'last'/'next'/'redo'" parse error. last/next/redo were only ever
# recognized at full-statement dispatch (parseStmt), not in
# parsePrimary (expression context), so they couldn't appear as a
# comma-expression operand. Found verbatim (the identical line) in
# two real scripts: /usr/bin/perlbug and /usr/bin/perlthanks:
# `$sendmail = $_, last if -e $_;` inside _probe_for_sendmail().
my $sendmail = "";
for (qw(a b c)) {
    $sendmail = $_, last if $_ eq 'b';
}
print "last_comma=$sendmail\n";

# next as a comma-expression operand
my @seen;
for my $i (1..5) {
    my $skip = ($i == 3);
    push(@seen, "pre-$i");
    $skip = 1, next if $i == 3;
    push(@seen, "post-$i");
}
print "next_comma=", join(",", @seen), "\n";

# redo as a comma-expression operand (bounded so it can't loop
# forever even if something regresses)
my $tries = 0;
my $retried;
for (1..1) {
    $tries++;
    $retried = 1, redo if $tries < 3;
}
print "redo_comma=tries:$tries retried:$retried\n";

# labeled next as a comma-expression operand (the exact real-world
# "break out of an inner loop, continue the outer one" idiom)
my $count = 0;
OUTER: for my $i (1..3) {
    for my $j (1..3) {
        $count++, next OUTER if $j == 2;
    }
}
print "labeled_next_comma=$count\n";

# labeled last as a comma-expression operand
my $lcount = 0;
OUTER2: for my $i (1..3) {
    for my $j (1..3) {
        $lcount++, last OUTER2 if $i == 2 && $j == 2;
    }
}
print "labeled_last_comma=$lcount\n";

# regression: plain statement-level last/next/redo (no comma, no
# modifier) must be completely unaffected
my @plain;
for my $i (1..5) {
    last if $i == 4;
    next if $i == 2;
    push(@plain, $i);
}
print "plain_stmt=", join(",", @plain), "\n";

print "done\n";
