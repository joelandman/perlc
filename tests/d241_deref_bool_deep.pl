use strict;
use warnings;
package P;
sub feed {
    my $self = shift;
    my $line = shift;
    my $paras = ($self->{'paras'} ||= []);
    if (!defined $line) {
        my $n = scalar @$paras;
        $self->_ponder();
        return "eof:$n";
    }
    if ($line eq "") {
        $self->{'last_blank'} = 1;
        return "blank:" . scalar(@$paras);
    }
    if ($self->{'last_blank'}) {
        $self->_ponder();
        push @$paras, [$line, { start => 1 }, "body"];
        $self->{'last_blank'} = 0;
        return "new:" . scalar(@$paras);
    }
    push @$paras, ["first", {}, $line];
    $self->{'last_blank'} = 0;
    return "first:" . scalar(@$paras);
}
sub _ponder {
    my $self = $_[0];
    my $paras;
    return unless @{$paras = $self->{'paras'}};
    print "ponder:", scalar(@$paras), ":", $paras->[0][0], "\n";
}
package main;
my $o = bless {}, 'P';
print $o->feed("=head1 NAME"), "\n";
print $o->feed(""), "\n";
print $o->feed("Widget"), "\n";
print $o->feed(undef), "\n";
my @named = (1);
print((@named ? "namedY" : "namedN"), "\n");
my $href = { k => 1, m => 2 };
print "keys=", scalar(%$href), "\n";
my $pref = $o->{'paras'};
print "count=", scalar(@$pref), " e0=", $pref->[0][0], " e2=", $pref->[0][2], "\n";
my $emptyh = {};
unless (%$emptyh) { print "emptyh\n"; }
if (%$href) { print "hashY\n"; }
