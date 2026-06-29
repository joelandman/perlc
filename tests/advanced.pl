use feature "say";
#!/usr/bin/perl
use 5.010;
use strict;
use warnings;

# --- array flattening: push @a, @b ---
my @a = (1, 2, 3);
my @b = (4, 5, 6);
push @a, @b;
say scalar @a;          # 6
say join(",", @a);      # 1,2,3,4,5,6

# --- my @merged = (@a, @b) ---
my @x = (1, 2, 3);
my @y = (7, 8);
my @merged = (@x, @y);
say scalar @merged;     # 5
say join(",", @merged); # 1,2,3,7,8

# --- unshift with array ---
my @arr = (4, 5, 6);
my @pre = (1, 2, 3);
unshift @arr, @pre;
say join(",", @arr);    # 1,2,3,4,5,6

# --- string ++ magic ---
my $s1 = "aa";  $s1++;  say $s1;   # ab
my $s2 = "az";  $s2++;  say $s2;   # ba
my $s3 = "zz";  $s3++;  say $s3;   # aaa
my $s4 = "Az";  $s4++;  say $s4;   # Ba
my $s5 = "Zz9"; $s5++;  say $s5;   # AAa0

# --- @ARGV and $0 ---
say scalar @ARGV;       # 2
say $ARGV[0];           # hello
say $ARGV[1];           # world

# --- anonymous subs ---
my $add = sub { my ($a, $b) = @_; return $a + $b; };
say $add->(3, 4);       # 7
say ref($add);          # CODE

# --- \&named sub ---
sub double { my ($n) = @_; return $n * 2; }
my $d = \&double;
say $d->(5);            # 10

# --- eval / $@ ---
eval { die "caught error\n"; };
say $@;                 # caught error\n

eval { my $ok = 1; };
say $@ eq "" ? "ok" : "fail";  # ok

eval { die "boom\n"; };
if ($@) { say "got: $@"; }      # got: boom\n

say "done";
