use Term::ReadLine;
my $t = Term::ReadLine->new("app");
print "impl=", $t->ReadLine, "\n";
print "class=", ref($t), "\n";
print "isa_stub=", ($t->isa("Term::ReadLine::Stub") ? 1 : 0), "\n";
print "in=", (defined $t->IN ? 1 : 0), "\n";
print "out=", (defined $t->OUT ? 1 : 0), "\n";
print "add=", ($t->addhistory("x") ? 1 : 0), "\n";
# Non-tty stdin: Stub returns undef
print "nontty=", (defined($t->readline("p> ")) ? 1 : 0), "\n";
print "done\n";
