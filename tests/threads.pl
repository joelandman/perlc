#!/usr/bin/perl
use strict;
use warnings;

use threads;

sub worker {
  return threads->tid;
}

my $t1 = threads->create(\&worker);
my $tid = $t1->join;
say $tid == 1;  # 1

1;