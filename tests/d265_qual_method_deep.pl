# D265: qualified method walks the named class's @ISA, skips the
# object's own override, and accepts a multi-level package name.
use strict;
use warnings;
package P265d;
sub meth { print "P:", ref($_[0]), ":", $_[1], "\n"; }
sub onlyp { print "onlyp:", $_[1], "\n"; }
package M265d;
our @ISA = qw(P265d);
package C265d;
our @ISA = qw(M265d);
sub meth { print "C\n"; }
package Other::Pkg265;
sub named { print "named:", ref($_[0]), ":", $_[1], "\n"; }
package main;
my $o = bless {}, "C265d";
$o->M265d::meth("z");
$o->P265d::meth("q");
$o->C265d::meth("skip");
$o->Other::Pkg265::named("n");
$o->P265d::onlyp("k");
print "done\n";
