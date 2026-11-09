use strict;
use warnings;

package D250;
sub setv {
    my ($self, $v) = @_;
    $self->{v} = $v;
    return "set:$v";
}
sub show {
    my ($self) = @_;
    return $self->{v};
}

package main;
my $o = bless { v => "z" }, "D250";
my $m = $o->can("setv");
print "can=", defined($m) ? "Y" : "N", "\n";
my $r = $o->$m("9");
print "r=$r v=", $o->show, "\n";
my $name = "show";
print "byname=", $o->$name, "\n";
my $no = $o->can("nope");
print "no=", defined($no) ? "Y" : "N", "\n";
