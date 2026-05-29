# threads::shared Hash Support Documentation

## Overview

The Perl compiler implements full support for `threads::shared` hash variables, allowing multiple threads to safely access and modify hash data structures with proper synchronization.

## Implementation Details

### Declaration
```perl
my %shared_hash : shared;
```

### Thread Safety
- Shared hashes use embedded mutexes for thread-safe access
- All modifications must be protected by `lock(%shared_hash)` 
- The same synchronization primitives apply as for shared scalars and arrays

### Usage Pattern
```perl
use threads;
use threads::shared;

my %shared_hash : shared;

# Multiple threads writing to different keys
my @threads;
for my $i (1..5) {
    push @threads, threads->create(sub {
        lock(%shared_hash);
        $shared_hash{"key_$i"} = $i * 10;
    });
}

# Wait for completion
for my $t (@threads) { $t->join(); }
```

## Key Features

1. **Thread-safe Access**: Multiple threads can safely read and write to shared hashes
2. **Locking Mechanism**: Requires explicit locking with `lock(%shared_hash)` for atomic operations
3. **Visibility**: Changes made by one thread are immediately visible to other threads
4. **Isolation**: Non-shared variables remain thread-local as expected

## Test Status

The implementation has been verified through the existing `threads.pl` test suite with the following test:
```
# Test 13: shared hash — multiple threads write distinct keys, all visible after join
my %shared_hash : shared;
# ... creates 5 threads that write to different keys
# Verifies all keys and values are correctly set after join
print "shared_hash_ok=yes"
```

## Example Usage

```perl
use threads;
use threads::shared;

my %counter : shared;

# Thread function that increments counter
sub increment_counter {
    lock(%counter);
    $counter{$_[0]} = ($counter{$_[0]} || 0) + 1;
}

# Create multiple threads
my @threads;
for my $i (1..10) {
    push @threads, threads->create(\&increment_counter, "thread_$i");
}

# Wait for completion
for my $t (@threads) { $t->join(); }

# Access final results
lock(%counter);
print "Counter values:\n";
for my $key (keys %counter) {
    print "$key: $counter{$key}\n";
}
unlock(%counter);
```

## Limitations

- Requires explicit locking for all access patterns
- Cannot perform complex operations without proper locking
- Performance considerations for high-contention scenarios

## References

This functionality is part of the broader threads::shared implementation documented in the compiler as:
- `my %hash : shared` - shared hash variables
- `lock(%hash)` - lock for thread-safe access  
- `unlock(%hash)` - unlock (automatically handled with block scope)