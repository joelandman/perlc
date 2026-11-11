# D264: a require that eval-requires itself runs once.
use lib 'tests/lib';
BEGIN {
    my $old = defined $ENV{PERL5LIB} ? $ENV{PERL5LIB} : '';
    $ENV{PERL5LIB} = ($old eq '') ? 'tests/lib' : "tests/lib:$old";
}
my $m = 'D264Self';
my $ok = eval "require $m;";
my $at = $@;
print "ok=", ($ok ? 1 : 0), " at=", ($at eq '' ? "empty" : "set"), "\n";
my $ok2 = eval "require $m;";
print "again=", ($ok2 ? 1 : 0), "\n";
