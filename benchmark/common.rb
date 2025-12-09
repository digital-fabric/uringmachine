# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'io-event'
  gem 'async'
  gem 'pg'
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
    threads:      [:threads,      "Threads"],
    thread_pool:  [:thread_pool,  "ThreadPool"],
    async_uring:  [:scheduler,    "Async uring"],
    async_epoll:  [:scheduler,    "Async epoll"],
    um_fs:        [:scheduler,    "UM FS"],
    um:           [:um,           "UM"],
    um_sqpoll:    [:um,           "UM sqpoll"]
  }

  def run_benchmarks(b)
    @@benchmarks.each do |sym, (doer, name)|
      b.report(name) { send(:"run_#{sym}") } if respond_to?(:"do_#{doer}")
    end
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
    fds.each { machine.close_async(it) }
    machine.snooze
  end
end

at_exit { UMBenchmark.new.benchmark }
