use strict;
use warnings;

package Acc;
sub install {
    no strict 'refs';
    my $pkg = caller();
    foreach my $attr (@_) {
        *{$pkg . '::' . $attr} = sub {
            (@_ == 1) ? $_[0]->{$attr} : ($_[0]->{$attr} = $_[1]);
        };
    }
}

package Widget;
our $VERSION = '9.1';
Acc::install('color', 'size');

package Child;
our @ISA = ('Widget');
our $VERSION = '1.2';

package P;
sub greet { return "static" }
no warnings 'redefine';
*greet = sub { return "dyn" };

package S;
sub ping { return "pong" }

package None;

package main;
my $o = bless {}, 'Widget';
$o->color(3);
$o->size(4);
print "color=", $o->color, " size=", $o->size, "\n";
print "set=", $o->color(9), "\n";
print "color2=", $o->color, "\n";
print "ver=", $o->VERSION, "\n";
print "cver=", Widget->VERSION, "\n";

my $c = bless {}, 'Child';
$c->color('red');
$c->size('big');
print "ccolor=", $c->color, " csize=", $c->size, " cver=", $c->VERSION, "\n";

my $p = bless {}, 'P';
print "greet=", $p->greet, "\n";

my $s = bless {}, 'S';
print "ping=", $s->ping, "\n";

my $n = bless {}, 'None';
print "nver=", (defined $n->VERSION ? $n->VERSION : "UNDEF"), "\n";
