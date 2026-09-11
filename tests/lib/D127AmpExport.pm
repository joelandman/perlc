package D127AmpExport;

# D127: real Pod::Usage.pm exports this way — `our @EXPORT = qw(&name);`
# with an explicit leading & sigil on the sub name inside the qw() list.
use Exporter ();
our @ISA = qw(Exporter);
our @EXPORT = qw(&greet);
our @EXPORT_OK = qw(&farewell);

sub greet { return "hello"; }
sub farewell { return "bye"; }

1;
