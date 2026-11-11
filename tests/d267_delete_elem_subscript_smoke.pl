#!/usr/bin/perl
# D267: delete $_[0]{key} deletes the hash element, including inside a call.
use strict;
use warnings;

sub grab {
    my $v = delete $_[0]{htext};
    print "v=$v n=", (exists $_[0]{htext} ? 1 : 0), " k=$_[0]{keep}\n";
}

my $h = { htext => "Hello", keep => 7 };
grab($h);

sub tag {
    return "I:" . $_[0];
}
my @box = ({ htext => "T", keep => 1 });
my $id = tag(delete $box[0]{htext});
print "id=$id n=", (exists $box[0]{htext} ? 1 : 0), " k=$box[0]{keep}\n";
