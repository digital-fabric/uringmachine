require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
end

require 'uringmachine/fiber_scheduler'
require 'securerandom'

system('rm /tmp/mutex_io_*') rescue nil

GROUPS = ENV['N']&.to_i || 10
WORKERS = 10
ITERATIONS = 10000

SIZE = 1024
DATA = "*" * SIZE

def run_threads
  # system('rm /tmp/mutex_io_*') rescue nil
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

def run_fiber_scheduler
  # system('rm /tmp/mutex_io_*') rescue nil
  machine = UM.new
  scheduler = UM::FiberScheduler.new(machine)
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
end

def run_um
  # system('rm /tmp/mutex_io_*') rescue nil
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

  machine.wait_fibers(fibers)
  fds.each { machine.close(it) }
end

Benchmark.bm do |x|
  GC.start
  x.report("Threads")           { run_threads }
  GC.start
  x.report("UM FiberScheduler") { run_fiber_scheduler }
  GC.start
  x.report("UM pure")           { run_um }
end
