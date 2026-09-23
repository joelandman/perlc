use Getopt::Std;
{
    @ARGV = qw(-a -b foo bar);
    my %o;
    my $ok = getopts("ab:", \%o);
    print "ok=$ok a=$o{a} b=$o{b} argv=@ARGV\n";
}
{
    @ARGV = qw(-abc val rest);
    my %o;
    getopts("abc:", \%o);
    print "cluster a=$o{a} b=$o{b} c=$o{c} argv=@ARGV\n";
}
{
    @ARGV = qw(-- leftover);
    my %o;
    getopts("a", \%o);
    print "dd=@ARGV\n";
}
print "done\n";
