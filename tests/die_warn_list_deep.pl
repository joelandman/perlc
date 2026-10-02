# Deep test for D178: die/warn LIST -- a comma-separated
# multi-argument form with no parens (`warn "a\n", "b\n", "c\n";`)
# -- was a hard "unexpected token ','" parse error. parseDieWarnBody
# only ever parsed a single expression, with no comma-continuation
# at all, unlike print/push and similar LIST-taking builtins. Found
# via a real /usr/bin/geteltorito script's multi-line `warn` call
# inside its usage() sub.
#
# Real Perl joins every list element with NO separator (exactly like
# a multi-arg print), not just evaluating and keeping the last one.
warn "w1\n", "w2\n", "w3\n";
print "after_warn\n";

eval { die "d1 ", "d2 ", "d3\n"; };
print "die_err=$@";

# regression: D102's die-REF behavior (a single reference argument,
# no comma) must preserve the actual reference, not stringify it
eval { die { code => 42, msg => "boom" }; };
print "ref_type=", ref($@), " code=", $@->{code}, "\n";

# regression: single-argument die/warn (the overwhelmingly common
# case) completely unaffected
eval { die "single arg\n"; };
print "single_die_err=$@";

# a comma-separated list where one element is a variable, not just
# string literals (the real-world geteltorito shape mixes variables
# and literals)
my $name = "World";
eval { die "Hello, ", $name, "!\n"; };
print "mixed_die_err=$@";

# multi-line, more than 3 elements (matching the real script's own
# scale more closely)
eval { die "a", "b", "c", "d", "e\n"; };
print "five_die_err=$@";

print "done\n";
