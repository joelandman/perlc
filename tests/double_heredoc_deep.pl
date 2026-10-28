# D193: stacked heredoc bodies, interpolation, a single heredoc, and << as shift.
my $a = <<'A' . <<'B';
aaa
A
bbb
B
print "[$a]";
my $x = "X";
my $s = <<"A" . <<"B";
$x-
A
${x}2
B
print "[$s]";
my $n = 1 ? <<END : <<END;
one
END
two
END
print "[$n]";
my $one = <<'Z';
only
Z
print $one;
print 1 << 5, "\n";
print __LINE__, "\n";
