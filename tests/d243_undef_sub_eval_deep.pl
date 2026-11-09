eval { Missing::Pkg::name("a", "b"); };
my $e = $@;
print "has=", ($e =~ /Undefined subroutine &Missing::Pkg::name called at \S+ line \d+\./) ? "Y" : "N", "\n";
print "nl=", (substr($e, -1) eq "\n") ? "Y" : "N", "\n";
$@ = "";
eval { die "boom\n"; };
print "die=[$@]";
eval { 1; };
print "clear=[$@]\n";
sub fine { return "ok"; }
eval { my $x = fine(); print "$x\n"; };
print "after=[$@]\n";
