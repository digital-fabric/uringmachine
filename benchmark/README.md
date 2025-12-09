# UringMachine Benchmarks

The following benchmarks measure the performance of UringMachine against stock
Ruby in a variety of scenarios. For each scenario, we compare three different
implementations:

- Thread-based concurrency using the stock Ruby I/O and synchronization classes.
- Fiber-based concurrency with the UringMachine fiber scheduler, using the
  stock Ruby I/O and synchronization classes.
- Fiber-based concurrency using the UringMachine low-level (pure) API.



## 1. I/O - Pipe

50 concurrent groups, where in each group we create a pipe with a pair of
threads/fibers writing/reading 1KB of data to the pipe.

```
              user     system      total        real
Threads   2.506197   3.088454   5.594651 (  4.956997)
Async FS  1.193954   0.419434   1.613388 (  1.613560)
UM FS     0.688240   0.307525   0.995765 (  0.996139)
UM pure   0.253324   0.330341   0.583665 (  0.583843)
```

## 2. I/O - Socketpair

50 concurrent groups, where in each group we create a unix socketpair with a
pair of threads/fibers writing/reading 1KB of data to the sockets.

```
user     system      total        real
Threads   2.320470   3.887775   6.208245 (  4.943793)
Async FS  0.508515   0.798268   1.306783 (  1.307060)
UM FS     0.497538   0.832940   1.330478 (  1.330843)
UM pure   0.207668   0.632624   0.840292 (  0.840562)
```

## 3. Mutex - CPU-bound

20 concurrent groups, where in each group we create a mutex and start 10 worker
threads/fibers locking the mutex and performing a Regexp match.

```
user     system      total        real
Threads   5.300555   0.035522   5.336077 (  5.325899)
Async FS  5.311512   0.004965   5.316477 (  5.317157)
UM FS     5.301865   0.000000   5.301865 (  5.302522)
UM pure   5.291983   0.008991   5.300974 (  5.301578)
```

## 4. Mutex - I/O-bound

N concurrent groups, where in each group we create a mutex, open a file and
start 10 worker threads/fibers locking the mutex and writing 1KB chunks to the
file.

```
N=1           user     system      total        real
Threads   0.353471   0.576720   0.930191 (  0.773777)
Async FS  0.273938   0.748782   1.022720 (  0.928015)
UM FS     0.321644   0.796376   1.118020 (  1.017612)
UM pure   0.209001   0.897841   1.106842 (  1.020961)

N=5           user     system      total        real
Threads   1.867092   3.483718   5.350810 (  4.196995)
Async FS  0.684154   1.491352   2.175506 (  1.200852)
UM FS     0.766727   1.954056   2.720783 (  1.293250)
UM pure   0.463424   1.996961   2.460385 (  1.138078)

N=10          user     system      total        real
Threads   3.836791   7.237707  11.074498 (  8.586606)
Async FS  1.170350   2.687634   3.857984 (  2.077960)
UM FS     1.328229   2.923621   4.251850 (  2.270382)
UM pure   0.660448   2.796605   3.457053 (  1.831333)

N=20          user     system      total        real
Threads   7.684380  15.012919  22.697299 ( 17.751588)
Async FS  2.517380   5.040245   7.557625 (  4.090871)
UM FS     3.007983   7.188207  10.196190 (  4.950258)
UM pure   1.184024   5.933565   7.117589 (  3.214988)

N=50          user     system      total        real
Threads  19.495570  37.259494  56.755064 ( 44.776187)
Async FS  8.373260  11.724548  20.097808 ( 11.588783)
UM FS    10.023142  20.535555  30.558697 ( 15.123302)
UM pure   2.427401  13.494526  15.921927 (  7.254758)

N=100         user     system      total        real
Threads  42.044358  80.442909 122.487267 ( 96.574176)
Async FS 26.199758  22.802559  49.002317 ( 31.232776)
UM FS    31.518132  44.427632  75.945764 ( 40.106880)
UM pure   5.007674  27.199925  32.207599 ( 15.012285)
```

## 5. Queue

20 concurrent groups, where in each group we create a queue, start 4 producer
threads/fibers that push items to the queue, and 8 consumer threads/fibers that
pull items from the queue.

```
              user     system      total        real
Threads   1.130497   0.060878   1.191375 (  1.182039)
Async FS  0.988763   0.026930   1.015693 (  1.015933)
UM FS     1.022337   0.020941   1.043278 (  1.043397)
UM pure   0.906505   0.079977   0.986482 (  0.986675)
```
