use feature "say";
#!/usr/bin/perl
use 5.010;
use strict;

my $file = "/tmp/perlc_fileio_test.txt";

# --- write ---
open(my $out, '>', $file) or die "Cannot write $file";
print $out "line one\n";
print $out "line two\n";
print $out "line three\n";
close($out);

# --- read line by line ---
open(my $in, '<', $file) or die "Cannot read $file";
while (my $line = <$in>) {
    chomp $line;
    say $line;
}
close($in);

# --- read all lines at once ---
open($in, '<', $file) or die "Cannot read $file";
my @lines = <$in>;
close($in);
say scalar @lines;      # 3
chomp @lines;
say $lines[0];          # line one
say $lines[2];          # line three

# --- append ---
open(my $app, '>>', $file) or die "Cannot append";
print $app "line four\n";
close($app);

open($in, '<', $file) or die;
my @all = <$in>;
close($in);
say scalar @all;        # 4

# --- eof ---
open($in, '<', $file) or die;
while (<$in>) {}
say eof($in) ? "eof" : "not eof";   # eof
close($in);

# --- 2-arg open ---
open(my $fh2, "<$file") or die "2-arg open failed";
my $first = <$fh2>;
chomp $first;
say $first;             # line one
close($fh2);

# --- print STDERR ---
print STDERR "stderr test\n";

# --- sprintf in print ---
printf $out "ignored\n";   # $out is closed, just testing parse
open(my $fout, '>', '/tmp/perlc_printf_test.txt') or die;
printf $fout "%s=%d\n", "answer", 42;
close($fout);
open(my $fin, '<', '/tmp/perlc_printf_test.txt') or die;
my $pline = <$fin>;
chomp $pline;
say $pline;    # answer=42
close($fin);

# cleanup
unlink $file;
unlink '/tmp/perlc_printf_test.txt';
say "done";
