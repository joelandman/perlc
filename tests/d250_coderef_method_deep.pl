use strict;
use warnings;

package D250p;
sub base {
    my ($self, $x) = @_;
    return "B$x";
}
package D250c;
our @ISA = ("D250p");
sub own {
    my ($self) = @_;
    return "own";
}
package main;

my $o = bless {}, "D250c";
my $via = $o->can("base");
print "inh=", $o->$via("1"), "\n";

sub apply {
    my ($self, $x) = @_;
    return $self->{k} . $x;
}
$o->{k} = "k";
my $c = \&apply;
print "cv=", $o->$c("!"), "\n";

my $n = 0;
sub tick { $n++; return "own" }
my $t = \&tick;
print "tick=", $o->$t(), " n=$n\n";
