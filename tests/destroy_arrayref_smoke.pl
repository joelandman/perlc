package Obj;
sub new {
    my ($class, @rest) = @_;
    bless [ @rest ], $class;
}
sub DESTROY {
    print "D\n";
}

package main;
{
    my $o = Obj->new(1);
    my $p = $o;
    $o = undef;
}
print "block\n";

my $a = Obj->new(2);
$a = undef;
print "undef\n";

my $b = Obj->new(3);
$b = Obj->new(4);
$b = undef;
print "overwrite\n";
print "smoke_done\n";