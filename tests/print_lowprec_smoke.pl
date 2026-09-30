# D147/D148 smoke: print/say/printf followed by low-precedence and/or, and
# `reverse` as a print argument (list context).
my @a=(5,4,3); my %h=(a=>1);
print reverse @a; print "\n";
print "x", reverse(@a), "y\n";
print reverse "abc"; print "\n";
print scalar reverse "abc"; print "\n";
print reverse("ab","cd"), "\n";
my $s = reverse "abc"; print "$s\n";
my $V="VAL";
print $V and 1; print "\n";
print $V or print "never"; print "\n";
print "A" and print "B"; print "\n";
print "C" and print "D" or print "E"; print "\n";
say_it() and print "F\n";
sub say_it { print "S"; 1 }
print "G\n" if 1;
print "H" and print "I\n" if 1;
open my $fh, ">", \my $buf or die "open: $!";
print $fh "to buf" or die "print failed";
close $fh; print "[$buf]\n";
printf "%s-%s", "p", "q" and print "!\n";
print not 0; print "\n";
print STDOUT "J" and print "K\n";
my $r = 0; print "L" and $r = 5; print " r=$r\n";
