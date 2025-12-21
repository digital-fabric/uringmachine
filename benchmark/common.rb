# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'io-event'
  gem 'async'
  gem 'pg'
  gem 'gvltools'
end

require 'uringmachine/fiber_scheduler'

class WorkerThreadPool
  def initialize(size)
    @size = size
    @queue = Queue.new
    setup_threads
    sleep 0.01 * @size
  end

  def queue(&block)
    @queue << block
  end

  def join
    @size.times { @queue << :stop }
    @threads.each(&:join)
  end

  def setup_threads
    @threads = @size.times.map {
      Thread.new do
        loop do
          job = @queue.shift
          break if job == :stop

          job.()
        end
      end
    }
  end
end

class UMBenchmark
  def initialize
    @thread_pool = WorkerThreadPool.new(10)
  end

  def benchmark
    Benchmark.bm { run_benchmarks(it) }
  end

  @@benchmarks = {
    # baseline:     [:baseline,     "No Concurrency"],
    # baseline_um:  [:baseline_um,  "UM no concurrency"],
    # thread_pool:  [:thread_pool,  "ThreadPool"],

    threads:        [:threads,      "Threads"],

    async_uring:    [:scheduler,    "Async uring"],
    async_uring_x2: [:scheduler_x,  "Async uring x2"],

    # async_epoll:    [:scheduler,    "Async epoll"],
    # async_epoll_x2: [:scheduler_x,  "Async epoll x2"],

    um_fs:          [:scheduler,    "UM FS"],
    um_fs_x2:       [:scheduler_x,  "UM FS x2"],

    um:             [:um,           "UM"],
    # um_sqpoll:      [:um,           "UM sqpoll"],
    um_x2:          [:um_x,         "UM x2"],
    um_x4:          [:um_x,         "UM x4"],
    um_x8:          [:um_x,         "UM x8"],
  }

  def run_benchmarks(b)
    STDOUT.sync = true
    @@benchmarks.each do |sym, (doer, name)|
      if respond_to?(:"do_#{doer}")
        STDOUT << "Running #{name}... "
        ts = nil
        b.report(name) {
          ts = measure_time { send(:"run_#{sym}") }
        }
        p ts
        cleanup
      end
    end
  end

  def cleanup
  end

  def run_baseline
    do_baseline
  end

  def run_baseline_um
    machine = UM.new(4096)
    do_baseline_um(machine)
  end

  def run_threads
    threads = []
    ios = []
    do_threads(threads, ios)
    threads.each(&:join)
    ios.each { it.close rescue nil }
  end

  def run_thread_pool
    ios = []
    do_thread_pool(@thread_pool, ios)
    @thread_pool.join
    ios.each { it.close rescue nil }
  end

  def run_async_uring
    selector ||= IO::Event::Selector::URing.new(Fiber.current)
    scheduler = Async::Scheduler.new(selector:)
    Fiber.set_scheduler(scheduler)
    ios = []
    scheduler.run { do_scheduler(scheduler, ios) }
    ios.each { it.close rescue nil }
  end

  def run_async_epoll
    selector ||= IO::Event::Selector::EPoll.new(Fiber.current)
    scheduler = Async::Scheduler.new(selector:)
    Fiber.set_scheduler(scheduler)
    ios = []
    scheduler.run { do_scheduler(scheduler, ios) }
    ios.each { it.close rescue nil }
  end

  def run_um_fs
    machine = UM.new
    scheduler = UM::FiberScheduler.new(machine)
    Fiber.set_scheduler(scheduler)
    ios = []
    do_scheduler(scheduler, ios)
    scheduler.join
    ios.each { it.close rescue nil }
  end

  def run_async_uring_x2
    threads  = 2.times.map do
      Thread.new do
        selector ||= IO::Event::Selector::URing.new(Fiber.current)
        worker_pool = Async::Scheduler::WorkerPool.new
        scheduler = Async::Scheduler.new(selector:, worker_pool:)
        Fiber.set_scheduler(scheduler)
        ios = []
        scheduler.run { do_scheduler_x(2, scheduler, ios) }
        ios.each { it.close rescue nil }
      end
    end
    threads.each(&:join)
  end

  def run_async_epoll_x2
    threads  = 2.times.map do
      Thread.new do
        selector ||= IO::Event::Selector::EPoll.new(Fiber.current)
        scheduler = Async::Scheduler.new(selector:)
        Fiber.set_scheduler(scheduler)
        ios = []
        scheduler.run { do_scheduler_x(2, scheduler, ios) }
        ios.each { it.close rescue nil }
      end
    end
    threads.each(&:join)
  end

  def run_um_fs_x2
    threads  = 2.times.map do
      Thread.new do
        machine = UM.new
        thread_pool = UM::BlockingOperationThreadPool.new(2)
        scheduler = UM::FiberScheduler.new(machine, thread_pool)
        Fiber.set_scheduler(scheduler)
        ios = []
        do_scheduler_x(2, scheduler, ios)
        scheduler.join
        ios.each { it.close rescue nil } 
      end
    end
    threads.each(&:join)
  end

  def run_um
    machine = UM.new(4096)
    fibers = []
    fds = []
    do_um(machine, fibers, fds)
    machine.await_fibers(fibers)
    fds.each { machine.close(it) }
  end

  def run_um_sqpoll
    machine = UM.new(4096, true)
    fibers = []
    fds = []
    do_um(machine, fibers, fds)
    machine.await_fibers(fibers)
    fds.each { machine.close(it) }
  end

  def run_um_x2
    threads  = 2.times.map do
      Thread.new do
        machine = UM.new(4096)
        fibers = []
        fds = []
        do_um_x(2, machine, fibers, fds)
        machine.await_fibers(fibers)
        fds.each { machine.close(it) }
      end
    end
    threads.each(&:join)
  end

  def run_um_x4
    threads  = 4.times.map do
      Thread.new do
        machine = UM.new(4096)
        fibers = []
        fds = []
        do_um_x(4, machine, fibers, fds)
        machine.await_fibers(fibers)
        fds.each { machine.close(it) }
      end
    end
    threads.each(&:join)
  end

  def run_um_x8
    threads  = 8.times.map do
      Thread.new do
        machine = UM.new(4096)
        fibers = []
        fds = []
        do_um_x(8, machine, fibers, fds)
        machine.await_fibers(fibers)
        fds.each { machine.close(it) }
      end
    end
    threads.each(&:join)
  end

  def measure_time
    GVLTools::GlobalTimer.enable
    t0s = [
      Process.clock_gettime(Process::CLOCK_MONOTONIC),
      Process.clock_gettime(Process::CLOCK_PROCESS_CPUTIME_ID),
      GVLTools::GlobalTimer.monotonic_time / 1_000_000_000.0
    ]
    yield
    t1s = [
      Process.clock_gettime(Process::CLOCK_MONOTONIC),
      Process.clock_gettime(Process::CLOCK_PROCESS_CPUTIME_ID),
      GVLTools::GlobalTimer.monotonic_time / 1_000_000_000.0
    ]
    {
      monotonic:  t1s[0] - t0s[0],
      cpu:        t1s[1] - t0s[1],
      gvl:        t1s[2] - t0s[2]
    }
  ensure
    GVLTools::GlobalTimer.disable
  end
end

at_exit { UMBenchmark.new.benchmark }
