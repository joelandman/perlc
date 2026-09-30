# D149 smoke: `xor` is logical exclusive-or, not `or`.
my $r;
$r = (1 xor 1); print "11=<$r>\n";
$r = (1 xor 0); print "10=<$r>\n";
$r = (0 xor 1); print "01=<$r>\n";
$r = (0 xor 0); print "00=<$r>\n";
print "yes\n" if 1 xor 0;
print "bad\n" if 1 xor 1;
$r = (0 or 1 and 0); print "or-and <$r>\n";
