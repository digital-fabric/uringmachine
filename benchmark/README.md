# UringMachine Benchmarks

The following benchmarks measure the performance of UringMachine against stock
Ruby in a variety of scenarios. For each scenario, we compare three different
implementations:

- *Threads*: thread-based concurrency using the stock Ruby I/O and
  synchronization classes.
  
- *Async FS*: fiber-based concurrency with the
  [Async](https://github.com/socketry/async) fiber scheduler, using the stock
  Ruby I/O and synchronization classes.
  
- *UM FS*: fiber-based concurrency with the UringMachine fiber scheduler, using
  the stock Ruby I/O and synchronization classes.
  
- *UM pure*: fiber-based concurrency using the UringMachine low-level (pure)
  API.
  
- *UM sqpoll*: the same as *UM pure* with [submission queue
  polling](https://unixism.net/loti/tutorial/sq_poll.html).

<img src="./chart.png">

## Observations:

- We see the stark difference between thread-based and fiber-based concurrency.
  For I/O-bound workloads, there's really no contest - and that's exactly why
  the fiber scheduler interface changes everything.

- The UringMachine fiber scheduler is in some cases faster than the Async fiber
  scheduler, but not in all. This might be because the Async FS does scheduling
  of fibers in plain Ruby, while the UMFS implements a runqueue in its
  C-extension.

- The UringMachine low-level API is faster to use in most cases, and its
  performance advantage grows with the level of concurrency.

- SQ polling provides a performance advantage in high-concurrency scenarios,
  depending on the context. It remains to be seen how it affects performance in
  real-world situations. 

## 1. I/O - Pipe

50 groups, where in each group we create a pipe with a pair of threads/fibers
writing/reading 1KB of data to the pipe.

```
C=50x2         user     system      total        real
Threads    2.501885   3.111840   5.613725 (  5.017991)
Async FS   1.189332   0.526275   1.715607 (  1.715726)
UM FS      0.715688   0.318851   1.034539 (  1.034723)
UM pure    0.241029   0.365079   0.606108 (  0.606308)
UM sqpoll  0.217577   0.634414   0.851991 (  0.593531)
```

## 2. I/O - Socketpair

50 concurrent groups, where in each group we create a unix socketpair with a
pair of threads/fibers writing/reading 1KB of data to the sockets.

```
N=50           user     system      total        real
Threads    2.372753   3.612468   5.985221 (  4.798625)
Async FS   0.516226   0.877822   1.394048 (  1.394266)
UM FS      0.521360   0.875674   1.397034 (  1.397327)
UM pure    0.239353   0.642498   0.881851 (  0.881962)
UM sqpoll  0.220933   1.021997   1.242930 (  0.976198)
```

## 3. Mutex - CPU-bound

20 concurrent groups, where in each group we create a mutex and start 10 worker
threads/fibers locking the mutex and performing a Regexp match.

```
N=20           user     system      total        real
Threads    5.348378   0.021847   5.370225 (  5.362117)
Async FS   5.519970   0.003964   5.523934 (  5.524536)
UM FS      5.505282   0.003983   5.509265 (  5.509840)
UM pure    5.607048   0.002991   5.610039 (  5.610749)
UM sqpoll  5.437836   5.418316  10.856152 (  5.443331)
```

## 4. Mutex - I/O-bound

N concurrent groups, where in each group we create a mutex, open a file and
start 10 worker threads/fibers locking the mutex and writing 1KB chunks to the
file.

```
N=1            user     system      total        real
Threads    0.044103   0.057831   0.101934 (  0.087204)
Async FS   0.050608   0.084449   0.135057 (  0.121300)
UM FS      0.030355   0.077069   0.107424 (  0.108146)
UM pure    0.024489   0.086201   0.110690 (  0.108023)
UM sqpoll  0.022752   0.225133   0.247885 (  0.136251)

N=5            user     system      total        real
Threads    0.214296   0.384078   0.598374 (  0.467425)
Async FS   0.085820   0.158782   0.244602 (  0.139766)
UM FS      0.064279   0.147278   0.211557 (  0.117488)
UM pure    0.036478   0.182950   0.219428 (  0.119745)
UM sqpoll  0.036929   0.347573   0.384502 (  0.160814)

N=10           user     system      total        real
Threads    0.435688   0.752219   1.187907 (  0.924561)
Async FS   0.126573   0.303704   0.430277 (  0.234900)
UM FS      0.128427   0.215204   0.343631 (  0.184074)
UM pure    0.065522   0.359659   0.425181 (  0.192385)
UM sqpoll  0.076810   0.477429   0.554239 (  0.210087)

N=20           user     system      total        real
Threads    0.830763   1.585299   2.416062 (  1.868194)
Async FS   0.291823   0.644043   0.935866 (  0.507887)
UM FS      0.226202   0.460401   0.686603 (  0.362879)
UM pure    0.120524   0.616274   0.736798 (  0.332182)
UM sqpoll  0.177150   0.849890   1.027040 (  0.284069)

N=50           user     system      total        real
Threads    2.124048   4.182537   6.306585 (  4.878387)
Async FS   0.897134   1.268629   2.165763 (  1.254624)
UM FS      0.733193   0.971821   1.705014 (  0.933749)
UM pure    0.226431   1.504441   1.730872 (  0.760731)
UM sqpoll  0.557310   2.107389   2.664699 (  0.783992)

N=100          user     system      total        real
Threads    4.420832   8.628756  13.049588 ( 10.264590)
Async FS   2.557661   2.532998   5.090659 (  3.179336)
UM FS      2.262136   1.912055   4.174191 (  2.523789)
UM pure    0.633897   2.793998   3.427895 (  1.612989)
UM sqpoll  1.119460   4.193703   5.313163 (  1.525968)

N=200         user     system      total        real
Threads    9.441905  19.173541  28.615446 ( 22.857740)
Async FS   9.063194   5.375939  14.439133 ( 10.156692)
UM FS      8.788661   4.124227  12.912888 (  8.967054)
UM pure    1.213050   5.659938   6.872988 (  3.387250)
UM sqpoll  1.435932   8.326375   9.762307 (  2.436268)
```

## 5. Queue

20 concurrent groups, where in each group we create a queue, start 5 producer
threads/fibers that push items to the queue, and 10 consumer threads/fibers that
pull items from the queue.

```
N=20           user     system      total        real
Threads    2.522270   0.125569   2.647839 (  2.638276)
Async FS   2.245917   0.044860   2.290777 (  2.291068)
UM FS      2.235130   0.000958   2.236088 (  2.236392)
UM pure    2.125827   0.225050   2.350877 (  2.351347)
UM sqpoll  2.044662   2.460344   4.505006 (  2.261502)
```
