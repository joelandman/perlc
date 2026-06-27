use feature "say";
#!/usr/bin/perl
use strict;
use warnings;

# --- math ---
say abs(-5);         # 5
say abs(3.7);        # 3.7
say int(3.9);        # 3
say int(-3.9);       # -3
printf "%.1f\n", sqrt(16);  # 4.0

# --- case ---
say uc("hello");     # HELLO
say lc("WORLD");     # world
say ucfirst("foo");  # Foo
say lcfirst("BAR");  # bAR

# --- index / rindex ---
say index("hello world", "world");   # 6
say index("hello world", "xyz");     # -1
say index("abcabc", "b", 2);         # 4
say rindex("hello world", "l");      # 9
say rindex("hello world", "l", 5);   # 3

# --- chr / ord ---
say chr(65);     # A
say ord("A");    # 65
say ord("abc");  # 97

# --- hex / oct ---
say hex("ff");     # 255
say hex("0xFF");   # 255
say oct("077");    # 63
say oct("0b1010"); # 10

# --- reverse ---
my @arr = (1, 2, 3, 4, 5);
my @rev = reverse @arr;
say join(",", @rev);           # 5,4,3,2,1
say scalar reverse("hello");   # olleh

# --- map ---
my @nums = (1, 2, 3, 4, 5);
my @doubled = map { $_ * 2 } @nums;
say join(",", @doubled);  # 2,4,6,8,10
my @strs = map { "n$_" } @nums;
say $strs[0];  # n1
say $strs[4];  # n5

# --- grep ---
my @evens = grep { $_ % 2 == 0 } @nums;
say join(",", @evens);  # 2,4
my @big = grep { $_ > 3 } @nums;
say join(",", @big);  # 4,5

# --- sort with comparator ---
my @sn = sort { $a <=> $b } (5, 3, 1, 4, 2);
say join(",", @sn);  # 1,2,3,4,5
my @sr = sort { $b <=> $a } @nums;
say join(",", @sr);  # 5,4,3,2,1
my @words = ("banana", "apple", "cherry");
my @sw = sort { $a cmp $b } @words;
say join(",", @sw);   # apple,banana,cherry
my @swr = sort { $b cmp $a } @words;
say join(",", @swr);  # cherry,banana,apple

# --- <=> and cmp ---
say(1 <=> 2);    # -1
say(2 <=> 2);    # 0
say(3 <=> 2);    # 1
say("a" cmp "b");  # -1
say("b" cmp "b");  # 0
say("c" cmp "b");  # 1

say "done";
