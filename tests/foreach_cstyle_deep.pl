# Deep test for D184: real Perl's `for` and `foreach` keywords are full
# synonyms, including the C-style three-clause form (`foreach (init;
# cond; step) { }`) -- previously only `for` ran the lookahead that
# detects a top-level `;` inside the parens to pick the C-style parse;
# `foreach` went straight to the list/foreach-variable parse, so a `;`
# inside its parens was a hard "expected ) but got ';'" parse error.
# Found via two separate real scripts in the same survey:
# /usr/share/doc/ppp/examples/scripts/lcp_rtt_dump and
# /usr/lib/llvm-22/libexec/ccc-analyzer, both spelling the C-style loop
# with `foreach` instead of `for`.

foreach (my $i = 0; $i < 3; $i++) {
    print "basic=$i\n";
}

# multiple comma-separated init/step clauses
foreach (my ($i, $j) = (0, 10); $i < 3; $i++, $j--) {
    print "multi=$i,$j\n";
}

# next/last inside the C-style foreach
foreach (my $i = 0; $i < 5; $i++) {
    next if $i == 1;
    last if $i == 3;
    print "nextlast=$i\n";
}

# no init clause
my $k = 0;
foreach (; $k < 2; $k++) {
    print "noinit=$k\n";
}

# regression: plain for(;;) C-style stays unaffected
for (my $m = 0; $m < 2; $m++) {
    print "for_cstyle=$m\n";
}

# regression: foreach-list form (both named-var and implicit $_) stays
# unaffected
foreach my $x (1, 2, 3) {
    print "list=$x\n";
}
foreach (4, 5) {
    print "implicit=$_\n";
}

# regression: foreach-list with a continue block stays unaffected
foreach my $w (1, 2) {
    print "cont_body=$w\n";
} continue {
    print "cont_block\n";
}

print "done\n";
