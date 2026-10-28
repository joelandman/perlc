# D197: unparenthesized multi-value return — `return '', '', 0`
# Real Perl: `return A, B, C` returns a list (A, B, C).
# perlc previously: "unexpected token ','" on the second comma.
# Found verbatim in /usr/bin/zipdetails.

sub f { return '', '', 0; }
my @r = f();
print join("|", @r), "\n";
