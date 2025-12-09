require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'io-event'
  gem 'async'
end

require 'uringmachine/fiber_scheduler'
require 'socket'

GROUPS = 50
ITERATIONS = 10000

SIZE = 1024
DATA = '*' * SIZE

def run_threads
  threads = []
  GROUPS.times do
    r, w = Socket.socketpair(:AF_UNIX, :SOCK_STREAM, 0)
    r.sync = true
    w.sync = true
    threads << Thread.new do
      ITERATIONS.times { w.send(DATA, 0) }
      w.close
    end
    threads << Thread.new do
      ITERATIONS.times { r.recv(SIZE) }
      r.close
    end
  end
  threads.each(&:join)
end

def run_async_fiber_scheduler
  # Thread.new do
    scheduler = Async::Scheduler.new
    Fiber.set_scheduler(scheduler)
    scheduler.run do
      GROUPS.times do
        r, w = Socket.socketpair(:AF_UNIX, :SOCK_STREAM, 0)
        r.sync = true
        w.sync = true
        Fiber.schedule do
          ITERATIONS.times { w.send(DATA, 0) }
          w.close
        end
        Fiber.schedule do
          ITERATIONS.times { r.recv(SIZE) }
          r.close
        end
      end
    end
  # end.join
end

def run_um_fiber_scheduler
  machine = UM.new
  scheduler = UM::FiberScheduler.new(machine)
  Fiber.set_scheduler(scheduler)
  GROUPS.times do
    r, w = Socket.socketpair(:AF_UNIX, :SOCK_STREAM, 0)
    r.sync = true
    w.sync = true
    Fiber.schedule do
      ITERATIONS.times { w.send(DATA, 0) }
      w.close
    end
    Fiber.schedule do
      ITERATIONS.times { r.recv(SIZE) }
      r.close
    end
  end
  scheduler.join
end

def run_um(sqpoll = nil)
  machine = UM.new(4096, sqpoll)
  fibers = []
  GROUPS.times do
    r, w = UM.socketpair(UM::AF_UNIX, UM::SOCK_STREAM, 0)
    fibers << machine.spin do
      ITERATIONS.times { machine.send(w, DATA, SIZE, UM::MSG_WAITALL) }
      machine.close_async(w)
    end
    fibers << machine.spin do
      ITERATIONS.times { machine.recv(r, +'', SIZE, 0) }
      machine.close_async(r)
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
