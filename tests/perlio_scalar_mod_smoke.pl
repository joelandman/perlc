use PerlIO::scalar;
my $s = "a\nb\n";
open my $fh, "<", \$s or die "open";
my $l1 = <$fh>;
print "l1=$l1";
print "done\n";
