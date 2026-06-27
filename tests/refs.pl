use feature "say";
#!/usr/bin/perl
use strict;
use warnings;

# --- scalar ref ---
my $x = 42;
my $ref = \$x;
say $$ref;        # 42
$$ref = 99;
say $x;           # 99

# --- array ref ---
my @arr = (1, 2, 3);
my $aref = \@arr;
say $aref->[0];   # 1
say $aref->[2];   # 3
$aref->[1] = 20;
say $arr[1];      # 20

# --- anon array ref ---
my $anon = [10, 20, 30];
say $anon->[0];   # 10
say $anon->[2];   # 30
push @$anon, 40;
say $anon->[3];   # 40

# --- hash ref ---
my %h = (a => 1, b => 2);
my $href = \%h;
say $href->{a};   # 1
$href->{c} = 3;
say $h{c};        # 3

# --- anon hash ref ---
my $ah = {x => 10, y => 20};
say $ah->{x};     # 10
say $ah->{y};     # 20

# --- ref() ---
say ref($aref);   # ARRAY
say ref($href);   # HASH
say ref(\$x);     # SCALAR

# --- chained subscripts ---
my $nested = {a => [1, 2, 3], b => {c => 42}};
say $nested->{a}->[1];   # 2
say $nested->{b}->{c};   # 42
