# Deep test for the bare $+ special variable (the "last bracket match
# of the last successful search pattern") -- was not implemented at
# all, neither as a bare expression nor inside string interpolation
# (distinct from %+'s already-working named-capture hash, $+{name}).
# Found as a side effect of D175's $] investigation.

"abc123" =~ /([a-z]+)(\d+)/;
print "interp=$+\n";
my $x = $+;
print "bare=$x\n";

# the highest-numbered PARTICIPATING group, not just the highest group
# in the pattern -- a later non-participating optional group must be
# skipped
"a" =~ /(a)(b)?/;
print "nonparticipating=$+\n";

"xyz" =~ /(x)(y)(z)/;
print "allmatch=$+\n";

# regression: %+ named-capture hash (the subscripted form) stays
# unaffected
"foo" =~ /(?<name>f)oo/;
print "named=$+{name}\n";

# regression: no-capture-group match leaves $+ as whatever it was
# before (not cleared to undef) -- just confirm no crash and $& still
# works independently
"zzz" =~ /zzz/;
print "amp=$&\n";

print "done\n";
