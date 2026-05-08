# perlc — Perl to Native Binary Compiler

A Perl compiler that translates Perl source to LLVM IR and links a C runtime to produce native executables. Written in C++17 using LLVM 18.

## Requirements

- `clang++` / `clang` (LLVM 18)
- `llvm-config-18`
- `libpcre2-8` (`apt install libpcre2-dev`)

## Build

```bash
make        # produces ./perlc
make test   # runs all test programs and prints their output
make clean
```

## Usage

```bash
./perlc program.pl -o output            # compile and link
./perlc program.pl -o out.ll --emit-ir  # dump LLVM IR instead of linking
```

## Supported Perl Features

### Variables

| Syntax | Description |
|--------|-------------|
| `$x`, `my $x` | Scalar variables |
| `@arr`, `my @arr` | Arrays |
| `%hash`, `my %hash` | Hashes |
| `$arr[i]` | Array element |
| `$hash{key}` | Hash element |
| `my ($a, $b) = @_` | List assignment / argument unpacking |

### Literals

- Integers, floats
- Single-quoted strings `'...'` (no interpolation)
- Double-quoted strings `"..."` (variable and `\n`/`\t` interpolation)
- `undef`

### Operators

- Arithmetic: `+`, `-`, `*`, `/`, `%`
- String: `.` (concat), `x` (repeat)
- Range: `..` (e.g. `1..10`)
- Comparison (numeric): `==`, `!=`, `<`, `>`, `<=`, `>=`
- Comparison (string): `eq`, `ne`, `lt`, `gt`, `le`, `ge`
- Logical: `&&`, `||`, `!`, `and`, `or`, `not`
- Increment/decrement: `++`, `--` (prefix and postfix)
- Compound assignment: `+=`, `-=`, `*=`, `/=`, `.=`
- Ternary: `? :`
- String repetition: `x`

### Control Flow

```perl
if ($x) { ... } elsif ($y) { ... } else { ... }
unless ($x) { ... }
while ($cond) { ... }
while (my $line = <$fh>) { ... }   # my-in-condition idiom
until ($cond) { ... }
do { ... } while ($cond);
do { ... } until ($cond);
for (my $i = 0; $i < 10; $i++) { ... }
foreach my $v (@arr) { ... }
foreach my $i (1..10) { ... }
last;   # break
next;   # continue
```

### Statement Modifiers

Any statement can be followed by a postfix modifier:

```perl
say "yes" if $x > 0;
say "no"  unless $found;
$i++      while $i < 10;
$j--      until $j == 0;
say $_    for @arr;
say $_    foreach (1, 2, 3);
```

### Subroutines

```perl
sub name {
    my ($a, $b) = @_;
    return $a + $b;
}
my $result = name(1, 2);
```

### I/O

```perl
print "text";
print "a", "b", "c";
say "text";          # print with newline
say $var;
printf "%s=%d\n", $key, $val;
sprintf "%05.2f", $n;
print STDERR "error\n";
```

### File I/O

```perl
open(my $fh, '>', "file.txt") or die "Cannot open: $!";
open(my $fh, '<', "file.txt") or die;
open(my $fh, '>>', "file.txt");    # append
open(my $fh, "<file.txt");         # 2-arg form

print $fh "text\n";
say   $fh "text";
printf $fh "%d\n", 42;

my $line  = <$fh>;                 # readline (scalar)
my @lines = <$fh>;                 # slurp all lines (array)

close($fh);
eof($fh);                          # true after last read

die "error message\n";             # print to STDERR and exit
unlink "file.txt";                 # delete file
```

### String Builtins

```perl
chomp($s);                    # remove trailing newline in-place
chomp(@arr);                  # chomp every element
length($s)                    # string length
substr($s, $offset)           # substring from offset
substr($s, $offset, $len)     # substring with length
$a . $b                       # concatenation
$s x $n                       # repetition
```

### Array Builtins

```perl
push @arr, $v;
pop @arr
shift @arr
unshift @arr, $v;
scalar @arr                   # length
join(", ", @arr)
join("-", 1..5)                # range in join
split(/sep/, $str)
sort @arr                     # lexicographic sort
```

### Hash Builtins

```perl
keys %hash
values %hash
exists $hash{key}
delete $hash{key}
scalar %hash                  # number of key-value pairs
```

### References

```perl
my $ref  = \$scalar;          # scalar reference
my $aref = \@array;           # array reference
my $href = \%hash;            # hash reference
my $aref = [1, 2, 3];         # anonymous array
my $href = {a => 1, b => 2};  # anonymous hash

$$ref                         # dereference scalar
@$aref                        # dereference array
%$href                        # dereference hash
$aref->[0]                    # arrow subscript (array)
$href->{key}                  # arrow subscript (hash)
ref($ref)                     # "SCALAR", "ARRAY", "HASH", or ""
```

### Regex (PCRE2)

```perl
$s =~ /pattern/flags          # match (true/false)
$s !~ /pattern/flags          # negated match
$s =~ /(\w+)/                 # with captures → $1, $2, ...
$s =~ s/pat/replacement/      # substitution
$s =~ s/pat/replacement/g     # global substitution
$s =~ s/(\w+)/[$1]/g          # substitution with capture backreferences

# /g iterator in while loop
while ($s =~ /(\w+)/g) { say $1; }

# /g list context
my @words = ($s =~ /(\w+)/g);
foreach my $m ($s =~ /(\w+)/g) { say $m; }

# split with regex
my @parts = split(/,+/, $csv);
```

Supported flags: `i` (case-insensitive), `g` (global/all matches), `s` (dot matches newline), `m` (multiline).

## Architecture

```
source.pl  →  Lexer  →  Parser  →  AST  →  Codegen  →  LLVM IR  →  clang-18  →  binary
                                                                          ↑
                                                                    runtime.c (linked in)
```

| File | Role |
|------|------|
| `src/lexer.h/cpp` | Context-aware tokenizer |
| `src/ast.h` | Node kinds (`NK` enum) and `Node` struct |
| `src/parser.h/cpp` | Recursive-descent parser |
| `src/codegen.h/cpp` | AST → LLVM IR via IRBuilder |
| `src/runtime.h/c` | C runtime: `PerlValue` tagged union and all operations |
| `src/main.cpp` | Driver: lex → parse → codegen → link |

### Runtime Value Model

All Perl values are heap-allocated `PerlValue` structs (tagged union):

```c
typedef struct PerlValue {
    PerlTag tag;        // UNDEF, INT, FLOAT, STRING, REF_SCALAR, REF_ARRAY, REF_HASH, FILEHANDLE
    union { long long ival; double fval; char *sval; void *pval; };
    long long matchpos; // /g iterator position
} PerlValue;
```
