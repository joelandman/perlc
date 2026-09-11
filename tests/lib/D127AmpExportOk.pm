package D127AmpExportOk;

# D127: explicit @EXPORT_OK-only import path, separate fixture from
# D127AmpExport.pm so requesting an explicit import here doesn't
# interact with that module's default @EXPORT semantics.
use Exporter ();
our @ISA = qw(Exporter);
our @EXPORT_OK = qw(&shout);

sub shout { return "SHOUT"; }

1;
