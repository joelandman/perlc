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

package main;
my $o = bless {}, 'Widget';
$o->color(3);
$o->size(4);
print "color=", $o->color, " size=", $o->size, "\n";
print "ver=", $o->VERSION, "\n";
print "cver=", Widget->VERSION, "\n";
