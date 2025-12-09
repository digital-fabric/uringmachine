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

<img src="./chart.png">

## 1. I/O - Pipe

50 concurrent groups, where in each group we create a pipe with a pair of
threads/fibers writing/reading 1KB of data to the pipe.

```
N=50          user     system      total        real
Threads   2.218838   2.729582   4.948420 (  4.450232)
Async FS  1.136269   0.458769   1.595038 (  1.595244)
UM FS     0.651478   0.327257   0.978735 (  0.979133)
UM pure   0.226837   0.341539   0.568376 (  0.568664)
```

## 2. I/O - Socketpair

50 concurrent groups, where in each group we create a unix socketpair with a
pair of threads/fibers writing/reading 1KB of data to the sockets.

```
N=50          user     system      total        real
Threads   2.038691   3.355664   5.394355 (  4.337242)
Async FS  0.526322   0.770983   1.297305 (  1.297580)
UM FS     0.469290   0.842309   1.311599 (  1.311979)
UM pure   0.227637   0.603944   0.831581 (  0.831811)
```

## 3. Mutex - CPU-bound

20 concurrent groups, where in each group we create a mutex and start 10 worker
threads/fibers locking the mutex and performing a Regexp match.

```
N=20          user     system      total        real
Threads   5.109329   0.016229   5.125558 (  5.117787)
Async FS  5.196394   0.006960   5.203354 (  5.204112)
UM FS     5.140855   0.002983   5.143838 (  5.144595)
UM pure   5.209638   0.008995   5.218633 (  5.219309)
```

## 4. Mutex - I/O-bound

N concurrent groups, where in each group we create a mutex, open a file and
start 10 worker threads/fibers locking the mutex and writing 1KB chunks to the
file.

```
N=1           user     system      total        real
Threads   0.035013   0.056987   0.092000 (  0.076400)
Async FS  0.036953   0.070906   0.107859 (  0.099738)
UM FS     0.026742   0.065379   0.092121 (  0.092953)
UM pure   0.019253   0.081535   0.100788 (  0.098651)

N=5           user     system      total        real
Threads   0.223255   0.312319   0.535574 (  0.418414)
Async FS  0.076403   0.156112   0.232515 (  0.131394)
UM FS     0.070267   0.119471   0.189738 (  0.105529)
UM pure   0.033473   0.173126   0.206599 (  0.112118)

N=10          user     system      total        real
Threads   0.413100   0.687134   1.100234 (  0.858972)
Async FS  0.120133   0.274076   0.394209 (  0.214851)
UM FS     0.106022   0.216041   0.322063 (  0.172295)
UM pure   0.065967   0.284208   0.350175 (  0.185191)

N=20          user     system      total        real
Threads   0.791722   1.434177   2.225899 (  1.753223)
Async FS  0.260528   0.509606   0.770134 (  0.422749)
UM FS     0.227727   0.405714   0.633441 (  0.337078)
UM pure   0.104541   0.591078   0.695619 (  0.312684)

N=50          user     system      total        real
Threads   2.050880   3.664665   5.715545 (  4.489188)
Async FS  0.818177   1.217570   2.035747 (  1.170686)
UM FS     0.737403   0.908713   1.646116 (  0.896816)
UM pure   0.273933   1.386275   1.660208 (  0.728375)

N=100         user     system      total        real
Threads   4.346155   7.886190  12.232345 (  9.652601)
Async FS  2.444899   2.346073   4.790972 (  3.001407)
UM FS     2.042372   1.855187   3.897559 (  2.359921)
UM pure   0.508573   2.822692   3.331265 (  1.519224)

N=200         user     system      total        real
Threads   9.083989  17.866147  26.950136 ( 21.491992)
Async FS  8.400413   4.923262  13.323675 (  9.398307)
UM FS     8.429383   3.542056  11.971439 (  8.465551)
UM pure   1.259075   5.250329   6.509404 (  3.208363)
```

## 5. Queue

20 concurrent groups, where in each group we create a queue, start 4 producer
threads/fibers that push items to the queue, and 8 consumer threads/fibers that
pull items from the queue.

```
N=20          user     system      total        real
Threads   1.221488   0.063794   1.285282 (  1.277677)
Async FS  1.100493   0.044960   1.145453 (  1.145690)
UM FS     1.058176   0.033955   1.092131 (  1.092379)
UM pure   1.024583   0.100029   1.124612 (  1.124821)
```
