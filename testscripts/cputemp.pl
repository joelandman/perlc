#!/usr/bin/env perl

# Fixed version using only supported features (opendir, readdir, open, readline, print, regex)
# Avoids tie, IO::Dir, complex statement modifiers, and my in tight expressions.

$| = 1;

my $hostname;
chomp($hostname = `hostname`);

print "#### sync:", time(), "\n";

my $dh;
opendir $dh, "/sys/class/hwmon";

my $hw;
while ($hw = readdir($dh)) {
    if ($hw =~ /hwmon/) {
        my $path = "/sys/class/hwmon/" . $hw;

        my $th;
        opendir $th, $path;

        my $f;
        while ($f = readdir($th)) {
            if ($f =~ /temp(\d+)_input/) {
                my $core = $1;
                my $fname = $path . "/" . $f;
                my $fh;
                if (open $fh, "<", $fname) {
                    my $temp;
                    chomp($temp = <$fh>);
                    close $fh;
                    printf "cputemp,core=%d,machine=%s coretemp=%.1f\n",
                        $core, $hostname, $temp / 1000.0;
                }
            }
        }
        closedir $th;
    }
}
closedir $dh;

print "done\n";
