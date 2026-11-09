use strict;
use warnings;
package P;
sub build {
    my $class = shift;
    my $self = bless { z => 1 }, $class;
    my %opts = @_;
    my @opts = map { ("opt_$_", $opts{$_}) } sort keys %opts;
    %$self = (%$self, @opts);
    return $self;
}
package main;
my $p = P->build(errors => "die", name => "Widget", section => 3);
for my $k (sort keys %$p) {
    print "$k=$p->{$k}\n";
}
