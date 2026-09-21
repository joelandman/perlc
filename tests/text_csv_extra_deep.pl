use Text::CSV_PP;

my $c = Text::CSV_PP->new({quote_char => undef, sep_char => ";"});
$c->combine("a", "b;c", "d");
print "noquote=[", $c->string, "]\n";
$c->parse("x;y;z");
print "noparse=[", join("|", $c->fields), "]\n";

open my $fh, "<", \"a,b\nc,d\n";
my $fromfh = Text::CSV_PP::csv(in => $fh);
print "fh=$fromfh->[1][1]\n";
close $fh;

my $g = Text::CSV_PP->new({binary=>1});
open my $eh, "<", \"x,y\n";
while (1) { my $r = $g->getline($eh); last unless $r; }
print "eof_diag=", (0+$g->error_diag == 2012 ? 1 : 0), "\n";
close $eh;

print "done\n";
