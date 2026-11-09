package M;
sub n { return scalar(@_); }
sub show {
    my ($self, $name) = splice @_, 0, 2;
    my $attr = shift;
    my $rest = @_ ? $_[0] : "NONE";
    return "$name|$rest|" . ref($attr);
}
package main;
my $s = bless {}, "M";
my $a = ["=head1", {k => 1}, "NAME"];
print "n=", $s->n(@$a), "\n";
print "s=", $s->show(@$a), "\n";
my @b = (10, 20);
print "named=", $s->n(@b), "\n";
print "one=", $s->n("z"), "\n";
