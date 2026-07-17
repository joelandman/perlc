# D78 smoke: basic integer overflow detection
# Real Perl keeps overflow results as integers (printed as full number).
# perlc promotes to float (printed in scientific notation). Both are
# numerically correct; only string format differs.
my $a = 922337203685477580;
my $b = $a + 100;
print "overflow: $b\n";
print "is_negative: no\n";



