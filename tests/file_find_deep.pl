use File::Find;
system("rm -rf /tmp/perlc_ffind_deep");
mkdir "/tmp/perlc_ffind_deep";
mkdir "/tmp/perlc_ffind_deep/sub";
mkdir "/tmp/perlc_ffind_deep/sub/deep";
open(my $f, ">", "/tmp/perlc_ffind_deep/f1.txt") or die; print $f "1\n"; close $f;
open($f, ">", "/tmp/perlc_ffind_deep/sub/f2.txt") or die; close $f;
open($f, ">", "/tmp/perlc_ffind_deep/sub/deep/f3.txt") or die; close $f;
# preorder: dir entry before its contents
my @pre;
find(sub { push @pre, $File::Find::name }, "/tmp/perlc_ffind_deep");
print "pre=[@pre]\n";
# postorder: contents before the dir entry
my @post;
finddepth(sub { push @post, $File::Find::name }, "/tmp/perlc_ffind_deep");
print "post=[@post]\n";
# no_chdir: $_ is the full path
my @nc;
find({ no_chdir => 1, wanted => sub { push @nc, $_ } }, "/tmp/perlc_ffind_deep/sub");
print "nc=[@nc]\n";
# prune: skip a subtree
my @pr;
find(sub {
    if (-d && $_ eq "sub") { $File::Find::prune = 1; }
    push @pr, $File::Find::name;
}, "/tmp/perlc_ffind_deep");
print "pr=[@pr]\n";
# multiple roots
my @mr;
find(sub { push @mr, $File::Find::name }, "/tmp/perlc_ffind_deep/sub", "/tmp/perlc_ffind_deep/f1.txt");
print "mr=[@mr]\n";
# dir/parent/name trio inside the callback
my @trio;
find(sub { push @trio, "($_|$File::Find::dir|$File::Find::name)" if -f }, "/tmp/perlc_ffind_deep");
print "trio=[@trio]\n";
# file size accumulation
my $total = 0;
find(sub { $total += -s $_ if -f $_ }, "/tmp/perlc_ffind_deep");
print "total=$total\n";
print "done\n";
