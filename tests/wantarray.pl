#!/usr/bin/perl
use strict;
use warnings;

sub list_ctx { return (1,2,3); }
sub scalar_ctx { return 'three'; }

my @list = list_ctx();
say scalar @list == 3;  # 1

my $scalar = scalar_ctx();
say $scalar eq 'three';  # 1

say wantarray ? 'list' : 'scalar';  # scalar (top-level)

sub test_wantarray {
  my $ctx = wantarray;
  return $ctx ? 'list' : 'scalar';
}
say test_wantarray() eq 'scalar';  # 1 (caller scalar ctx)

1;