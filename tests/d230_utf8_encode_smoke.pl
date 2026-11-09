use strict;
use warnings;
my $s = "\x{FEFF}";
print "before=", length($s), "\n";
my $r = utf8::encode($s);
my $hex = "";
for (my $i = 0; $i < length($s); $i++) { $hex .= sprintf("%02x", ord(substr($s, $i, 1))); }
print "r=", (defined $r ? "D" : "U"), " after=", length($s), " hex=$hex\n";
my $b = "abc";
utf8::encode($b);
print "b=$b\n";
my $ok = utf8::decode($s);
print "ok=", ($ok ? 1 : 0), " len=", length($s), "\n";
