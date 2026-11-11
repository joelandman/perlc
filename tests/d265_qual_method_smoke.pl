# D265: $obj->Pkg::method starts at Pkg and keeps the object as invocant.
package P265;
sub meth { print "P:", ref($_[0]), ":", $_[1], "\n"; }
package C265;
our @ISA = qw(P265);
sub meth { print "C\n"; }
package main;
my $o = bless {}, "C265";
$o->P265::meth("z");
print "done\n";
