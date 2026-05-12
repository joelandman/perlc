#!/usr/bin/env perl

use strict;
use IO::Dir;
use IO::Handle;
use Fcntl;
use Getopt::Long;

### this next line is ABSOLUTELY CRITICAL for correct functionality
$| = 1;
### Yes, it tells Perl not to buffer IO.
### otherwise the pipeing to stdio/stderr doesn't work correctly
### and this whole effort fails to generate any output


my ($path,$fpath,$hostname,$statm,$status,@lines,$line,$k,$v,$field,$s);
my ($d,$de,$ce,%dir,$name,$ee,$temp,$core,$p,$fl,$ps,$proc,@fields,$ts);
my ($cmd,$names,@n);

$path   = '/proc';
chomp($hostname = `hostname`);
GetOptions ("names|n=s" => \$names );
@n =  split(/[\ \, \:]/,$names) if (defined($names));
#printf "n=%s\n",join("_",@n);

@fields = qw(Name Pid PPid VmSize VmPeak VmData VmRSS Uid cmdline);
printf "#%s\n",join(",","timestamp",@fields);
while (1) {

    $ts = time;
    tie %dir, 'IO::Dir', $path;
    foreach $de (sort keys %dir) {
        next if ($de !~ /\d+/); #skip anything not specifically a process ID
        $fpath = join('/',$path,$de);
        $fl = sprintf('%s/%s',$fpath,'status');
        undef $ps;
        $status = &_get_contents($fl);
        foreach $line (split(/\n/,$status)) {
            if ($line =~ /^(.*?):\s+(.*)/) {
                $k=$1;
                $v=$2;
                $ps->{$k}=$v;
            }
        }

        if (@n && (grep {!/$ps->{Name}/} @n))
          {
            next;  
          }
        foreach $k (qw(Name State Pid PPid Uid Gid Threads)) {
            $proc->{$de}->{$k} = $ps->{$k};
        }
        foreach $k (qw(VmPeak VmSize VmData VmRSS)) {
            $proc->{$de}->{$k} =$ps->{$k} * 1024;
        }
        $fl = sprintf('%s/%s',$fpath,'cmdline');
        undef $ps;
        $cmd = &_get_contents($fl);
        $proc->{$de}->{cmdline}=$cmd;

    }
    untie %dir;
    foreach my $de (sort {$a <=> $b } (keys %{$proc})) {
        $s=$ts;
        foreach $field (@fields) {
            $s = join(",",$s,$proc->{$de}->{$field});
        }
        $s =~ s/^\,//g;
        printf "%s\n",$s;
        undef $s;
    }

    sleep(1);
}
printf "done\n";


sub _get_contents {
    my $fname = shift;
    my ($data,$read,$ifh);
    sysopen($ifh, $fname, O_RDONLY);
    $read = sysread $ifh,$data,4096;
    chomp($data);
    close($ifh);
    return $data;
}
