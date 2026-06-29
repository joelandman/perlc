use feature "say";
#!/usr/bin/perl
use 5.010;
use strict;
use warnings;

# --- chop ---
my $s = "hello";
my $c = chop($s);
say $c;   # o
say $s;   # hell

# --- warn (to stderr, should not appear on stdout) ---
warn "this goes to stderr\n";
say "after warn";  # after warn

# --- qw() ---
my @words = qw(apple banana cherry);
say scalar @words;    # 3
say $words[0];        # apple
say $words[2];        # cherry

my @nums = qw(1 2 3 4 5);
say join(",", @nums);  # 1,2,3,4,5

# --- splice ---
my @arr = (1, 2, 3, 4, 5);
my @removed = splice(@arr, 1, 2);
say join(",", @arr);      # 1,4,5
say join(",", @removed);  # 2,3

my @arr2 = (1, 2, 3, 4, 5);
splice(@arr2, 1, 2, 10, 20, 30);
say join(",", @arr2);  # 1,10,20,30,4,5

# --- array slice ---
my @data = (10, 20, 30, 40, 50);
my @slice = @data[1, 3];
say join(",", @slice);  # 20,40

# --- hash slice ---
my %h = (a => 1, b => 2, c => 3, d => 4);
my @hslice = @h{qw(a c)};
say join(",", @hslice);  # 1,3

# --- $ENV ---
$ENV{PERLC_TEST} = "hello_env";
my $ev = $ENV{PERLC_TEST};
say $ev;  # hello_env

# --- file tests ---
my $tmpfile = "/tmp/perlc_ft_test.txt";
open(my $fh, ">", $tmpfile) or die "open: $!";
print $fh "test\n";
close($fh);

say -e $tmpfile ? "exists" : "missing";   # exists
say -f $tmpfile ? "file" : "not_file";    # file
say -d $tmpfile ? "dir" : "not_dir";      # not_dir
say -d "/tmp"   ? "is_dir" : "no_dir";    # is_dir
say -e "/no_such_file_xyz" ? "yes" : "no"; # no
unlink $tmpfile;
say -e $tmpfile ? "still" : "gone";       # gone

# --- system ---
my $rc = system("true");
say $rc;   # 0
my $rc2 = system("false");
say $rc2;  # 1 (or nonzero)

# --- backtick ---
my $out = `echo hello`;
chomp($out);
say $out;  # hello

my $lines = `printf 'x\ny\nz\n'`;
chomp($lines);
my @lns = split(/\n/, $lines);
say scalar @lns;  # 3

say "done";
