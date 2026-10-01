my %h = (a => 1);
my $ref = \%h;
{
    local $ref->{a} = 99;
    print "inside=$h{a}\n";
}
print "outside=$h{a}\n";
