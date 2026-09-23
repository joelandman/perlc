use IPC::Open2;
use IPC::Open3;
use Symbol;

{
    my ($r, $w);
    open2($r, $w, "/bin/echo", "ping");
    my $line = <$r>;
    chomp $line;
    print "open2_echo=$line\n";
    close $r; close $w;
}

{
    my ($r, $w);
    open2($r, $w, "printf 'abc\\n'");
    my $line = <$r>;
    chomp $line;
    print "open2_sh=$line\n";
    close $r; close $w;
}

{
    my ($in, $out, $err);
    $err = Symbol::gensym();
    open3($in, $out, $err, "/bin/sh", "-c", "echo out; echo err >&2");
    close $in if $in;
    my $o = <$out>;
    my $e = <$err>;
    chomp $o; chomp $e;
    print "open3_out=$o\n";
    print "open3_err=$e\n";
    close $out; close $err;
}

print "done\n";
