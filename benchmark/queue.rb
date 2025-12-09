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
PRODUCERS = 4
CONSUMERS = 8
ITEMS = 100000

def run_threads
  threads = []
  GROUPS.times do
    queue = Queue.new
    PRODUCERS.times do
      threads << Thread.new do
        ITEMS.times { queue << rand(1000) }
        CONSUMERS.times { queue << :stop }
      end
    end
    CONSUMERS.times do
      threads << Thread.new do
        loop do
          item = queue.shift
          break if item == :stop

          item * rand(1000)
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
      queue = Queue.new
      PRODUCERS.times do
        Fiber.schedule do
          ITEMS.times { queue << rand(1000) }
          CONSUMERS.times { queue << :stop }
        end
      end
      CONSUMERS.times do
        Fiber.schedule do
          loop do
            item = queue.shift
            break if item == :stop

            item * rand(1000)
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
    queue = Queue.new
    PRODUCERS.times do
      Fiber.schedule do
        ITEMS.times { queue << rand(1000) }
        CONSUMERS.times { queue << :stop }
      end
    end
    CONSUMERS.times do
      Fiber.schedule do
        loop do
          item = queue.shift
          break if item == :stop

          item * rand(1000)
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
    queue = UM::Queue.new
    PRODUCERS.times do
      fibers << machine.spin do
        ITEMS.times { machine.push(queue, rand(1000)) }
        CONSUMERS.times { machine.push(queue, :stop) }
      end
    end
    CONSUMERS.times do
      fibers << machine.spin do
        loop do
          item = machine.shift(queue)
          break if item == :stop

          item * rand(1000)
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
end
