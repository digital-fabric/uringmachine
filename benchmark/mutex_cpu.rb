require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'io-event'
  gem 'async'
end

require 'uringmachine/fiber_scheduler'

GROUPS = 20
WORKERS = 10
ITERATIONS = 10000

STR = "foobar" * 100
RE = /foo(.+)$/

def run_threads
  threads = []
  GROUPS.times do
    mutex = Mutex.new
    WORKERS.times do
      threads << Thread.new do
        ITERATIONS.times do
          mutex.synchronize do
            STR.match(RE)
          end
        end
      end
    end
  end
  threads.each(&:join)
end

def run_async_fiber_scheduler
  scheduler = Async::Scheduler.new
  Fiber.set_scheduler(scheduler)
  scheduler.run do
    GROUPS.times do
      mutex = Mutex.new
      WORKERS.times do
        Fiber.schedule do
          ITERATIONS.times do
            mutex.synchronize do
              STR.match(RE)
            end
          end
        end
      end
    end
  end
end

def run_um_fiber_scheduler
  machine = UM.new
  scheduler = UM::FiberScheduler.new(machine)
  Fiber.set_scheduler(scheduler)

  GROUPS.times do
    mutex = Mutex.new
    WORKERS.times do
      Fiber.schedule do
        ITERATIONS.times do
          mutex.synchronize do
            STR.match(RE)
          end
        end
      end
    end
  end

  scheduler.join
end

def run_um(sqpoll = nil)
  machine = UM.new(4096, sqpoll)
  fibers = []

  GROUPS.times do
    mutex = UM::Mutex.new
    WORKERS.times do
      fibers << machine.spin do
        ITERATIONS.times do
          machine.synchronize(mutex) do
            STR.match(RE)
          end
        end
      end
    end
  end

  machine.await_fibers(fibers)
end

Benchmark.bm do |x|
  x.report("Threads")   { run_threads }
  x.report("Async FS")  { run_async_fiber_scheduler }
  x.report("UM FS")     { run_um_fiber_scheduler }
  x.report("UM pure")   { run_um }
  x.report("UM sqpoll") { run_um(true) }
end
