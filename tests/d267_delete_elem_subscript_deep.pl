#!/usr/bin/perl
# D267: chained delete on an array or hash element, including the
# Pod::Simple::XHTML `_end_head` shape.
use strict;
use warnings;

package T;
sub idify {
    my ($self, $t) = @_;
    return "I:$t";
}
sub endh {
    my $h = delete $_[0]{in_head};
    my $id = $_[0]->idify(delete $_[0]{htext});
    push @{ $_[0]{to_index} }, [$h, $id, delete $_[0]{'hhtml'}];
    my $row = $_[0]{to_index}[0];
    print "row=$row->[0]|$row->[1]|$row->[2]\n";
    print "in=", (exists $_[0]{in_head} ? 1 : 0), "\n";
    print "ht=", (exists $_[0]{htext} ? 1 : 0), "\n";
    print "hh=", (exists $_[0]{'hhtml'} ? 1 : 0), "\n";
    print "keep=$_[0]{keep}\n";
    my $miss = delete $_[0]{nope};
    print defined($miss) ? "miss=$miss\n" : "missundef\n";
}

package main;
my $o = bless {
    in_head => 2,
    htext => "Title",
    hhtml => "<b>T</b>",
    keep => "yes",
    to_index => [],
}, "T";
$o->endh();

my @rows = (
    { name => "a", n => 1 },
    { name => "b", cells => [10, 20] },
);
my $nm = delete $rows[0]{name};
print "nm=$nm leftn=$rows[0]{n} namen=", (exists $rows[0]{name} ? 1 : 0), "\n";
my $cell = delete $rows[1]{cells}[0];
print "cell=$cell c1=$rows[1]{cells}[1]\n";

my %h = (k => [5, 6, 7]);
my $mid = delete $h{k}[1];
print "mid=$mid z=$h{k}[0] two=$h{k}[2]\n";

my $inner = delete $rows[1]{cells};
print "inner0=$inner->[1]\n";
print "cellsgone=", (exists $rows[1]{cells} ? 1 : 0), "\n";
