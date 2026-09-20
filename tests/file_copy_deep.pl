use File::Copy;
mkdir "/tmp/perlc_fcopy_deep";
my $dir = "/tmp/perlc_fcopy_deep";
# fh -> fh copy preserves content
open(S, ">", "$dir/s.txt") or die; print S "alpha\nbeta\ngamma\n"; close S;
open(A, "<", "$dir/s.txt"); open(B, ">", "$dir/b.txt");
my $r = copy(*A, *B);
print "fh2fh=$r\n"; close A; close B;
local $/;
open(my $in, "<", "$dir/b.txt"); my $txt = <$in>; close $in;
print "content=[$txt]\n";
# fh -> path
open(C, "<", "$dir/s.txt");
print "fh2path=", copy(*C, "$dir/c.txt"), "\n"; close C;
print "c_size=", -s "$dir/c.txt", "\n";
# path -> fh
open(D, ">", "$dir/d.txt");
print "path2fh=", copy("$dir/s.txt", *D), "\n"; close D;
print "d_size=", -s "$dir/d.txt", "\n";
# self-copy warns + returns 0, target intact
my $r2 = copy("$dir/s.txt", "$dir/s.txt");
print "self=$r2 size=", -s "$dir/s.txt", "\n";
# two handles on the same file warn with glob names
open(E, "<", "$dir/s.txt"); open(F, "<", "$dir/s.txt");
my $r3 = copy(*E, *F);
print "same=$r3\n"; close E; close F;
# copy into an existing directory appends basename
mkdir "$dir/sub";
print "intodir=", copy("$dir/s.txt", "$dir/sub"), "\n";
print "sub_file=", (-e "$dir/sub/s.txt" ? 1 : 0), " orig=", (-e "$dir/s.txt" ? 1 : 0), "\n";
# move into an existing directory appends basename
print "movedir=", move("$dir/c.txt", "$dir/sub"), "\n";
print "sub_c=", (-e "$dir/sub/c.txt" ? 1 : 0), " c_gone=", (-e "$dir/c.txt" ? 1 : 0), "\n";
# failure cases
print "copy_dir=", copy("$dir/sub", "$dir/sub2"), " err=[$!]\n";
print "move_missing=", move("$dir/absent.txt", "$dir/x.txt"), " err=[$!]\n";
# larger file integrity
open(L, ">", "$dir/big.txt"); print L "0123456789" x 25000; close L;
print "big=", copy("$dir/big.txt", "$dir/big2.txt"), "\n";
print "big_size=", -s "$dir/big2.txt", "\n";
print "done\n";
