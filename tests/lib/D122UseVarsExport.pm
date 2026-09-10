package D122UseVarsExport;

# D122: real core File::Path.pm declares its exports this way — `use
# vars qw(...)` followed by a bare (no `our`) assignment — rather than
# `our @EXPORT_OK = qw(...)`. scanExports() previously only recognized
# the `our`-prefixed form.
use Exporter ();
use vars qw($VERSION @ISA @EXPORT @EXPORT_OK);
@ISA       = qw(Exporter);
@EXPORT    = qw(always_exported);
@EXPORT_OK = qw(explicitly_imported);

sub always_exported { return "always"; }
sub explicitly_imported { return "explicit"; }

1;
