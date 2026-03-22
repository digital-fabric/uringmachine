# frozen_string_literal: true

require_relative './common'
require 'socket'
require 'openssl'
require 'localhost/authority'

GROUPS = 48
ITERATIONS = 5000

SIZE = 1 << 14
DATA = '*' * SIZE

class UMBenchmark
  def server_ctx
    @server_ctx ||= Localhost::Authority.fetch.server_context
  end

  def ssl_wrap(sock, ctx)
    OpenSSL::SSL::SSLSocket.new(sock, ctx).tap { it.sync_close = true }
  end

  def ssl_socketpair(machine)
    sock1, sock2 = Socket.socketpair(:AF_UNIX, :SOCK_STREAM, 0)
    ssl1 = ssl_wrap(sock1, server_ctx)
    ssl2 = ssl_wrap(sock2, OpenSSL::SSL::SSLContext.new)

    if !machine
      t = Thread.new { ssl1.accept rescue nil }
      ssl2.connect
      t.join
    else
      machine.ssl_set_bio(ssl1)
      machine.ssl_set_bio(ssl2)
      f = machine.spin { ssl1.accept rescue nil }
      ssl2.connect
      machine.join(f)
    end
    [ssl1, ssl2]
  end

  def do_threads(threads, ios)
    GROUPS.times do
      r, w = ssl_socketpair(nil)
      threads << Thread.new do
        ITERATIONS.times { w.write(DATA) }
        w.close
      end
      threads << Thread.new do
        ITERATIONS.times { r.readpartial(SIZE) }
        r.close
      end
    end
  end

  def do_thread_pool(thread_pool, ios)
    GROUPS.times do
      r, w = ssl_socketpair(nil)
      r.sync = true
      w.sync = true
      ios << r << w
      ITERATIONS.times {
        thread_pool.queue { w.write(DATA) }
        thread_pool.queue { r.readpartial(SIZE) }
      }
    end
  end

  def do_scheduler(scheduler, ios)
    GROUPS.times do
      r, w = ssl_socketpair(nil)
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
  end

  def do_scheduler_x(div, scheduler, ios)
    (GROUPS/div).times do
      r, w = ssl_socketpair(nil)
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
  end

  def do_um(machine, fibers, fds)
    GROUPS.times do
      r, w = ssl_socketpair(machine)
      fibers << machine.spin do
        ITERATIONS.times { machine.ssl_write(w, DATA, SIZE) }
        machine.close_async(w)
      end
      fibers << machine.spin do
        ITERATIONS.times { machine.ssl_read(r, +'', SIZE) }
        machine.close_async(r)
      end
    end
  end

  def do_um_x(div, machine, fibers, fds)
    (GROUPS/div).times do
      r, w = ssl_socketpair(machine)
      fibers << machine.spin do
        ITERATIONS.times { machine.ssl_write(w, DATA, SIZE) }
        machine.close_async(w)
      end
      fibers << machine.spin do
        ITERATIONS.times { machine.ssl_read(r, +'', SIZE) }
        machine.close_async(r)
      end
    end
  end
end
