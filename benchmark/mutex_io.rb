require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'io-event'
  gem 'async'
end

require 'uringmachine/fiber_scheduler'
require 'securerandom'
require 'fileutils'

GROUPS = ENV['N']&.to_i || 10
WORKERS = 10
ITERATIONS = 10000

puts "N=#{GROUPS}"

SIZE = 1024
DATA = "*" * SIZE

class MethodCallAuditor
  attr_reader :calls

  def initialize(target)
    @target = target
    @calls = []
  end

  def respond_to?(sym, include_all = false) = @target.respond_to?(sym, include_all)

  def method_missing(sym, *args, &block)
    res = @target.send(sym, *args, &block)
    @calls << ({ sym:, args:, res:})
    res
  rescue Exception => e
    @calls << ({ sym:, args:, res: e})
    raise
  end

  def last_call
    calls.last
  end
end

def run_threads
  threads = []
  ios = []
  GROUPS.times do
    mutex = Mutex.new
    ios << (f = File.open("/tmp/mutex_io_threads_#{SecureRandom.hex}", 'w'))
    f.sync = true
    WORKERS.times do
      threads << Thread.new do
        ITERATIONS.times do
          mutex.synchronize do
            f.write(DATA)
          end
        end
      end
    end
  end
  threads.each(&:join)
  ios.each(&:close)
end

def run_async_fiber_scheduler
  scheduler = Async::Scheduler.new
  # scheduler = MethodCallAuditor.new(scheduler)
  Fiber.set_scheduler(scheduler)
  ios = []
  count = 0
  scheduler.run do
    GROUPS.times do
      mutex = Mutex.new
      ios << (f = File.open("/tmp/mutex_io_fiber_scheduler_#{SecureRandom.hex}", 'w'))
      f.sync = true
      WORKERS.times do
        Fiber.schedule do
          ITERATIONS.times do
            mutex.synchronize do
              count += f.write(DATA)
            end
          end
        end
      end
    end
  end
  ios.each(&:close)
  # pp scheduler.calls.map { it[:sym] }.tally
end

def run_um_fiber_scheduler
  machine = UM.new
  scheduler = UM::FiberScheduler.new(machine)
  # scheduler = MethodCallAuditor.new(scheduler)
  Fiber.set_scheduler(scheduler)
  ios = []
  count = 0

  GROUPS.times do
    mutex = Mutex.new
    ios << (f = File.open("/tmp/mutex_io_fiber_scheduler_#{SecureRandom.hex}", 'w'))
    f.sync = true
    WORKERS.times do
      Fiber.schedule do
        ITERATIONS.times do
          mutex.synchronize do
            count += f.write(DATA)
          end
        end
      end
    end
  end

  scheduler.join
  ios.each(&:close)
  # pp scheduler.calls.map { it[:sym] }.tally
end

def run_um
  machine = UM.new
  fibers = []
  fds = []
  count = 0

  GROUPS.times do
    mutex = UM::Mutex.new
    fds << (fd = machine.open("/tmp/mutex_io_um_#{SecureRandom.hex}", UM::O_CREAT | UM::O_WRONLY))
    WORKERS.times do
      fibers << machine.spin do
        ITERATIONS.times do
          machine.synchronize(mutex) do
            machine.write(fd, DATA)
          end
        end
      end
    end
  end

  machine.await_fibers(fibers)
  fds.each { machine.close(it) }
end

Benchmark.bm do |x|
  `rm -f /tmp/mutex*`; GC.start
  x.report("Threads")   { run_threads }
  `rm -f /tmp/mutex*`; GC.start
  x.report("Async FS")  { run_async_fiber_scheduler }
  `rm -f /tmp/mutex*`; GC.start
  x.report("UM FS")     { run_um_fiber_scheduler }
  `rm -f /tmp/mutex*`; GC.start
  x.report("UM pure")   { run_um }
  `rm -f /tmp/mutex*`; GC.start
end
