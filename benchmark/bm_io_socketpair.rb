# frozen_string_literal: true

require_relative './common'
require 'socket'

GROUPS = 50
ITERATIONS = 10000

SIZE = 1024
DATA = '*' * SIZE

class UMBenchmark
  def do_threads(threads, ios)
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
  end

  def do_thread_pool(thread_pool, ios)
    GROUPS.times do
      r, w = Socket.socketpair(:AF_UNIX, :SOCK_STREAM, 0)
      r.sync = true
      w.sync = true
      ios << r << w
      ITERATIONS.times {
        thread_pool.queue { w.send(DATA, 0) }
        thread_pool.queue { r.recv(SIZE) }
      }
    end
  end

  def do_scheduler(scheduler, ios)
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

  def do_um(machine, fibers, fds)
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
  end
end
