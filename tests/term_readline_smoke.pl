use Term::ReadLine;
my $t = Term::ReadLine->new("app");
print "impl=", $t->ReadLine, "\n";
print "class=", ref($t), "\n";
print "in=", (defined $t->IN ? 1 : 0), "\n";
print "done\n";
