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
                       user     system      total        real     ratio
Threads            3.180942   4.535893   7.716835 (  6.661512)    x1.00
UM FiberScheduler  0.734797   0.401159   1.135956 (  1.136021)    x0.17
UM pure            0.146786   0.321765   0.468551 (  0.468793)    x0.07
```

## 2. I/O - Socketpair

50 concurrent groups, where in each group we create a unix socketpair with a
pair of threads/fibers writing/reading 1KB of data to the sockets.

```
                       user     system      total        real     ratio
Threads            3.087872   5.371877   8.459749 (  6.658642)    x1.00
UM FiberScheduler  0.579182   1.017451   1.596633 (  1.597015)    x0.24
UM pure            0.118848   0.612396   0.731244 (  0.731465)    x0.11
```

## 3. Mutex - CPU-bound

20 concurrent groups, where in each group we create a mutex and start 10 worker
threads/fibers locking the mutex and performing a Regexp match.

```
                       user     system      total        real     ratio
Threads            5.517281   0.037554   5.554835 (  5.543819)    x1.00
UM FiberScheduler  5.198372   0.003999   5.202371 (  5.203005)    x0.94
UM pure            5.362594   0.005000   5.367594 (  5.368282)    x0.97
```

## 4. Mutex - I/O-bound

N concurrent groups, where in each group we create a mutex, open a file and
start 10 worker threads/fibers locking the mutex and writing 1KB chunks to the
file.

```
N=1                    user     system      total        real     ratio
Threads            0.464248   0.892285   1.356533 (  1.108107)    x1.00
UM FiberScheduler  0.419482   1.145587   1.565069 (  1.429551)    x1.29
UM pure            0.228935   1.307773   1.536708 (  1.414426)    x1.28

N=5                    user     system      total        real     ratio
Threads            2.753522   5.243440   7.996962 (  6.247786)    x1.00
UM FiberScheduler  0.723096   2.405801   3.128897 (  1.405862)    x0.22
UM pure            0.277712   1.967339   2.245051 (  0.994354)    x0.16

N=10                   user     system      total        real     ratio
Threads            5.493555  10.854305  16.347860 ( 12.862785)    x1.00
UM FiberScheduler  1.286965   4.099696   5.386661 (  2.417395)    x0.19
UM pure            0.382762   3.154509   3.537271 (  1.514130)    x0.12

N=20                   user     system      total        real     ratio
Threads           10.970734  22.493584  33.464318 ( 26.346881)    x1.00
UM FiberScheduler  2.498935   5.898825   8.397760 (  4.450860)    x0.17
UM pure            0.579116   4.977388   5.556504 (  2.839390)    x0.11

N=50                   user     system      total        real     ratio
Threads           27.517430  58.524771  86.042201 ( 67.544292)    x1.00
UM FiberScheduler  8.898559  15.072599  23.971158 ( 13.258086)    x0.20
UM pure            1.220973  13.224007  14.444980 (  6.151113)    x0.09

N=100                  user     system      total        real     ratio
Threads           58.513644 124.297334 182.860781 (144.413083)    x1.00
UM FiberScheduler 26.336488  43.101414  70.939939 ( 36.352986)    x0.25
UM pure            2.223913  30.475928  34.008375 ( 14.956937)    x0.10
```

## 5. Queue

20 concurrent groups, where in each group we create a queue, start 4 producer
threads/fibers that push items to the queue, and 8 consumer threads/fibers that
pull items from the queue.

```
                       user     system      total        real     ratio
Threads            1.606373   0.055943   1.662316 (  1.649027)    x1.00
UM FiberScheduler  1.033577   0.029969   1.063546 (  1.063966)    x0.64
UM pure            0.893669   0.083983   0.977652 (  0.977839)    x0.59
```
