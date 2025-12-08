require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
end

require 'uringmachine/fiber_scheduler'

GROUPS = 50
ITERATIONS = 10000

SIZE = 1024
DATA = '*' * SIZE

def run_threads
  threads = []
  GROUPS.times do
    r, w = IO.pipe
    r.sync = true
    w.sync = true
    threads << Thread.new do
      ITERATIONS.times { w.write(DATA) }
      w.close
    end
    threads << Thread.new do
      ITERATIONS.times { r.readpartial(SIZE) }
      r.close
    end
  end
  threads.each(&:join)
end

def run_fiber_scheduler
  machine = UM.new
  scheduler = UM::FiberScheduler.new(machine)
  Fiber.set_scheduler(scheduler)
  GROUPS.times do
    r, w = IO.pipe
    r.sync = true
    w.sync = true
    Fiber.schedule do
      ITERATIONS.times { w.write(DATA) }
      w.close
    end
    Fiber.schedule do
      ITERATIONS.times { r.readpartial(SIZE) }
      r.close
    end
  end
  scheduler.join
end

def run_um
  machine = UM.new
  fibers = []
  GROUPS.times do
    r, w = UM.pipe
    fibers << machine.spin do
      ITERATIONS.times { machine.write(w, DATA) }
      machine.close_async(w)
    end
    fibers << machine.spin do
      ITERATIONS.times { machine.read(r, +'', SIZE) }
      machine.close_async(r)
    end
  end
  machine.wait_fibers(fibers)
end

Benchmark.bm do |x|
  x.report("Threads")           { run_threads }
  x.report("UM FiberScheduler") { run_fiber_scheduler }
  x.report("UM pure")           { run_um }
end
