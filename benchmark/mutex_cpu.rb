require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
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

def run_fiber_scheduler
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

def run_um
  machine = UM.new
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

  machine.wait_fibers(fibers)
end

Benchmark.bm do |x|
  x.report("Threads")           { run_threads }
  x.report("UM FiberScheduler") { run_fiber_scheduler }
  x.report("UM pure")           { run_um }
end
