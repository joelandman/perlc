#!/usr/bin/perl
use strict;
use warnings;

# ── /x regex flag ────────────────────────────────────────────────────────────
my $str = "hello world 42";
if ($str =~ / hello   # greeting word
              \s+
              (\w+)   # captured name
            /x) {
    print "rx_x: $1\n";          # world
} else {
    print "rx_x: FAIL\n";
}

# /x with substitution
my $t = "foo   bar";
(my $u = $t) =~ s/foo   # first
                  \s+
                  bar   # second
                 /baz/x;
print "rx_x_s: $u\n";            # baz

# /ix combined
if ("Hello" =~ / hello /ix) { print "rx_ix: ok\n" } else { print "rx_ix: FAIL\n" }

# ── redo ─────────────────────────────────────────────────────────────────────
# redo restarts the current iteration without re-evaluating the condition
my $count = 0;
my $redo_fires = 0;
for my $i (1..3) {
    $count++;
    if ($i == 2 && $redo_fires == 0) {
        $redo_fires++;
        redo;          # re-run iteration 2 once more
    }
}
print "redo: count=$count redo_fires=$redo_fires\n";  # count=4 redo_fires=1

# redo in while loop
my $n = 0;
my $extra = 0;
while ($n < 3) {
    $n++;
    if ($n == 2 && $extra == 0) {
        $extra++;
        redo;   # re-execute body without re-checking $n < 3 or incrementing $n
    }
}
print "redo_while: n=$n extra=$extra\n";  # n=3 extra=1

# ── unshift @{$ref} ──────────────────────────────────────────────────────────
my @arr = (3, 4, 5);
my $ref = \@arr;

unshift @{$ref}, 1, 2;
print "unshift_ref: @arr\n";           # 1 2 3 4 5

# unshift @$ref form
my @arr2 = (30, 40);
my $ref2 = \@arr2;
unshift @$ref2, 10, 20;
print "unshift_scalar_ref: @arr2\n";   # 10 20 30 40

# unshift into nested hash ref
my %h = (list => [7, 8, 9]);
unshift @{$h{list}}, 5, 6;
my @hlist = @{$h{list}};
print "unshift_href: @hlist\n";        # 5 6 7 8 9

# ── do { BLOCK } as expression ───────────────────────────────────────────────
my $val = do { 1 + 2 };
print "do_block: $val\n";             # 3

my $msg = do {
    my $x = "hello";
    $x . " world";
};
print "do_block_str: $msg\n";         # hello world

# do block with conditionals
my $abs = do {
    my $v = -5;
    $v < 0 ? -$v : $v;
};
print "do_block_cond: $abs\n";        # 5

# do block in list context
my @items = (do { 1 + 1 }, do { 2 + 2 }, do { 3 + 3 });
print "do_block_list: @items\n";      # 2 4 6

# do block as function argument
sub double { $_[0] * 2 }
my $r = double(do { 6 + 1 });
print "do_block_arg: $r\n";           # 14

print "done\n";
