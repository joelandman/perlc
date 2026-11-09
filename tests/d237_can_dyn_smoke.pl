#!/usr/bin/perl
# D237: can() sees a dyn-sub method, including through @ISA.
use strict;
use warnings;
package P;
sub install {
    no strict 'refs';
    *{__PACKAGE__ . '::ping'} = sub { return "pong" };
}
install();
package C;
our @ISA = ('P');
package main;
my $o = bless {}, 'C';
print $o->can('ping') ? "yes\n" : "no\n";
print $o->ping, "\n";
print defined($o->can('missing')) ? "bad\n" : "nomiss\n";
