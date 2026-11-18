package E::NixFoo;
use strict;
use warnings;
use Exporter;
our @ISA    = qw(Exporter);
our @EXPORT = qw(foo);

sub foo { return 42; }

# Custom unimport: `no E::NixFoo qw(foo);` removes `foo` from the caller's
# namespace for the rest of the enclosing block (the canonical
# delete ${"$pkg\::"}{$n} shape real Perl uses). perlc models import() as a
# package-scoped compile-time map, so this mirrors that: the bareword
# `foo()` no longer resolves unqualified after the `no`.
sub unimport {
    my ($class, @names) = @_;
    no strict 'refs';
    my $pkg = caller();
    for my $n (@names) {
        delete ${"$pkg\::"}{$n};
    }
}

1;
