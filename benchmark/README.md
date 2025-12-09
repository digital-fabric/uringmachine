# UringMachine Benchmarks

The following benchmarks measure the performance of UringMachine against stock
Ruby in a variety of scenarios. For each scenario, we compare three different
implementations:

- Thread-based concurrency using the stock Ruby I/O and synchronization classes.
- Fiber-based concurrency with the [Async]() fiber scheduler, using the
  stock Ruby I/O and synchronization classes.
- Fiber-based concurrency with the UringMachine fiber scheduler, using the
  stock Ruby I/O and synchronization classes.
- Fiber-based concurrency using the UringMachine low-level (pure) API.

## 1. I/O - Pipe

50 concurrent groups, where in each group we create a pipe with a pair of
threads/fibers writing/reading 1KB of data to the pipe.

```
              user     system      total        real
Threads   2.232506   2.848687   5.081193 (  4.524141)
Async FS  1.262320   0.509936   1.772256 (  1.772287)
UM FS     0.716842   0.344866   1.061708 (  1.061823)
UM pure   0.266204   0.313543   0.579747 (  0.579908)
```

## 2. I/O - Socketpair

50 concurrent groups, where in each group we create a unix socketpair with a
pair of threads/fibers writing/reading 1KB of data to the sockets.

```
user     system      total        real
Threads   2.224307   3.332567   5.556874 (  4.445827)
Async FS  0.548417   0.797211   1.345628 (  1.345725)
UM FS     0.530902   0.835632   1.366534 (  1.366779)
UM pure   0.263900   0.671938   0.935838 (  0.936050)
```

## 3. Mutex - CPU-bound

20 concurrent groups, where in each group we create a mutex and start 10 worker
threads/fibers locking the mutex and performing a Regexp match.

```
user     system      total        real
Threads   5.173511   0.024183   5.197694 (  5.191095)
Async FS  5.330880   0.001984   5.332864 (  5.333474)
UM FS     5.439579   0.001996   5.441575 (  5.442224)
UM pure   5.476145   0.001997   5.478142 (  5.478693)
```

## 4. Mutex - I/O-bound

N concurrent groups, where in each group we create a mutex, open a file and
start 10 worker threads/fibers locking the mutex and writing 1KB chunks to the
file.

```
N=1           user     system      total        real
Threads   0.369724   0.530234   0.899958 (  0.745285)
Async FS  0.457296   0.687135   1.144431 (  0.991240)
UM FS     0.338470   0.712723   1.051193 (  0.988989)
UM pure   0.179378   0.903781   1.083159 (  1.001157)

N=5           user     system      total        real
Threads   1.868051   3.328609   5.196660 (  4.114922)
Async FS  0.963134   1.517727   2.480861 (  1.399078)
UM FS     0.871332   1.622975   2.494307 (  1.364419)
UM pure   0.406606   1.730224   2.136830 (  1.161408)

N=10          user     system      total        real
Threads   3.884542   6.932104  10.816646 (  8.540959)
Async FS  1.579248   2.711656   4.290904 (  2.372181)
UM FS     1.681060   3.144787   4.825847 (  2.543084)
UM pure   0.727527   3.388077   4.115604 (  1.886537)

N=20          user     system      total        real
Threads   7.665221  14.253493  21.918714 ( 17.399964)
Async FS  3.233115   5.051460   8.284575 (  4.640110)
UM FS     3.401095   6.802358  10.203453 (  5.340720)
UM pure   1.243642   5.880823   7.124465 (  3.251009)

N=50          user     system      total        real
Threads  20.227708  36.868640  57.096348 ( 45.237544)
Async FS  9.933987  11.500114  21.434101 ( 12.808607)
UM FS    11.467407  20.341159  31.808566 ( 16.475792)
UM pure   2.998017  14.315403  17.313420 (  8.044796)

N=100         user     system      total        real
Threads  43.310533  80.064367 123.374900 ( 98.103144)
Async FS 27.729002  22.427138  50.156140 ( 32.489543)
UM FS    31.888624  44.079206  75.967830 ( 40.256637)
UM pure   5.724054  27.344135  33.068189 ( 15.793288)
```

## 5. Queue

20 concurrent groups, where in each group we create a queue, start 4 producer
threads/fibers that push items to the queue, and 8 consumer threads/fibers that
pull items from the queue.

```
              user     system      total        real
Threads   1.186719   0.051169   1.237888 (  1.233542)
Async FS  1.136423   0.034915   1.171338 (  1.171468)
UM FS     1.147243   0.018894   1.166137 (  1.166354)
UM pure   1.141208   0.105116   1.246324 (  1.246514)
```
