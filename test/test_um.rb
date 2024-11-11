# frozen_string_literal: true

require_relative 'helper'
require 'socket'

class SchedulingTest < UMBaseTest
  def test_schedule_and_yield
    buf = []
    main = Fiber.current
    f = Fiber.new do |x|
      buf << [21, x]
      machine.schedule(main, 21)
      buf << 22
      x = machine.yield
      buf << [23, x]
    end

    buf << 11
    machine.schedule(f, 11)
    buf << 12
    x = machine.yield
    buf << [13, x]

    assert_equal [11, 12, [21, 11], 22, [13, 21]], buf
  end

  class CustomError < Exception
  end

  def test_schedule_exception
    buf = []
    f = Fiber.new do
      # this should raise
      machine.yield
    rescue Exception => e
      buf << e
      machine.yield
    end
    
    machine.schedule(f, nil)
    # start the f fiber
    machine.snooze

    # f fiber has yielded
    e = CustomError.new
    machine.schedule(f, e)
    machine.snooze

    assert_equal [e], buf
  end

  def test_schedule_exception2
    main = Fiber.current
    e = CustomError.new
    f = Fiber.new do
      machine.schedule(main, e)
      machine.yield
    end
    
    machine.schedule(f, nil)
    t0 = monotonic_clock

    # the call to schedule means an op is checked out
    assert_equal 0, machine.pending_count
    begin
      machine.sleep(1)
    rescue Exception => e2
    end
    assert_equal 0, machine.pending_count
    t1 = monotonic_clock

    assert_equal e2, e
    assert_in_range 0..0.1, t1 - t0
  end

  class TOError < RuntimeError; end

  def test_timeout
    buf = []
    begin
      buf << 1
      machine.timeout(0.01, TOError) do
        buf << 2
        machine.sleep(1)
        buf << 3
      end
      buf << 4
    rescue => e
      buf << 5
    end

    assert_equal 0, machine.pending_count
    assert_equal [1, 2, 5], buf
    assert_kind_of TOError, e
  end

  def test_timeout_with_raising_block
    e = nil
    begin
      machine.timeout(0.1, TOError) do
        raise 'hi'
      end
    rescue => e
    end

    assert_equal 1, machine.pending_count
    machine.sleep(0.01) # wait for cancelled CQEs
    assert_equal 0, machine.pending_count

    assert_kind_of RuntimeError, e
    assert_equal 'hi', e.message
  end

  def test_timeout_with_nothing_blocking
    v = machine.timeout(0.1, TOError) { 42 }

    assert_equal 42, v

    assert_equal 1, machine.pending_count
    machine.sleep 0.01 # wait for cancelled CQE
    assert_equal 0, machine.pending_count
  end

  class TO2Error < RuntimeError; end
  class TO3Error < RuntimeError; end

  def test_timeout_nested
    e = nil
    buf = []
    begin
      machine.timeout(0.04, TOError) do
        machine.timeout(0.02, TO2Error) do
          machine.timeout(0.03, TO3Error) do
            buf << machine.pending_count
            machine.sleep(1)
          end
        end
      end
    rescue => e
    end

    assert_equal 2, machine.pending_count
    machine.sleep(0.01) # wait for cancelled CQEs
    assert_equal 0, machine.pending_count

    assert_kind_of TO2Error, e
    assert_equal [3], buf
  end
end

class SleepTest < UMBaseTest
  def test_sleep
    t0 = monotonic_clock
    assert_equal 0, machine.pending_count
    res = machine.sleep(0.1)
    assert_equal 0, machine.pending_count
    t1 = monotonic_clock
    assert_in_range 0.09..0.13, t1 - t0
    assert_equal 0.1, res
  end

  class C; end

  def test_sleep_interrupted
    t0 = monotonic_clock
    ret = machine.timeout(0.03, C) do
      machine.sleep 1
    end
    t1 = monotonic_clock
    assert_in_range 0.02..0.04, t1 - t0
    assert_kind_of C, ret
  end

  class D < RuntimeError; end

  def test_sleep_with_timeout
    t0 = monotonic_clock
    ret = begin
      machine.timeout(0.03, D) do
        machine.sleep 1
      end
    rescue => e
      e
    end
    t1 = monotonic_clock
    assert_in_range 0.02..0.04, t1 - t0
    assert_kind_of D, ret
  end
end

class ReadTest < UMBaseTest
  def test_read
    r, w = IO.pipe
    w << 'foobar'

    buf = +''
    assert_equal 0, machine.pending_count
    res = machine.read(r.fileno, buf, 3)
    assert_equal 0, machine.pending_count
    assert_equal 3, res
    assert_equal 'foo', buf

    buf = +''
    res = machine.read(r.fileno, buf, 128)
    assert_equal 3, res
    assert_equal 'bar', buf

    w.close
    buf = +''
    res = machine.read(r.fileno, buf, 128)
    assert_equal 0, res
    assert_equal '', buf
  end

  def test_read_bad_fd
    _r, w = IO.pipe

    assert_raises(Errno::EBADF) do
      machine.read(w.fileno, +'', 8192)
    end
    assert_equal 0, machine.pending_count
  end

  def test_read_with_buffer_offset
    buffer = +'foo'
    r, w = IO.pipe
    w << 'bar'

    result = machine.read(r.fileno, buffer, 100, buffer.bytesize)
    assert_equal 3, result
    assert_equal 'foobar', buffer
  end

  def test_read_with_negative_buffer_offset
    buffer = +'foo'

    r, w = IO.pipe
    w << 'bar'

    result = machine.read(r.fileno, buffer, 100, -1)
    assert_equal 3, result
    assert_equal 'foobar', buffer

    buffer = +'foogrr'

    r, w = IO.pipe
    w << 'bar'

    result = machine.read(r.fileno, buffer, 100, -4)
    assert_equal 3, result
    assert_equal 'foobar', buffer
  end
end

class ReadEachTest < UMBaseTest
  def test_read_each
    r, w = IO.pipe
    bufs = []

    bgid = machine.setup_buffer_ring(4096, 1024)
    assert_equal 0, bgid

    f = Fiber.new do
      w << 'foo'
      machine.sleep 0.02
      w << 'bar'
      machine.sleep 0.02
      w << 'baz'
      machine.sleep 0.02
      w.close
      machine.yield
    end
    
    machine.schedule(f, nil)

    machine.read_each(r.fileno, bgid) do |buf|
      bufs << buf
    end

    assert_equal ['foo', 'bar', 'baz'], bufs
    assert_equal 0, machine.pending_count
  end

  # send once and close write fd
  def test_read_each_raising_1
    r, w = IO.pipe

    bgid = machine.setup_buffer_ring(4096, 1024)
    assert_equal 0, bgid

    w << 'foo'
    w.close

    e = nil
    begin
      machine.read_each(r.fileno, bgid) do |buf|
        raise 'hi'
      end
    rescue => e
    end

    assert_kind_of RuntimeError, e
    assert_equal 'hi', e.message
    assert_equal 0, machine.pending_count
  end

  # send once and leave write fd open
  def test_read_each_raising_2
    r, w = IO.pipe

    bgid = machine.setup_buffer_ring(4096, 1024)
    assert_equal 0, bgid

    w << 'foo'

    e = nil
    begin
      machine.read_each(r.fileno, bgid) do |buf|
        raise 'hi'
      end
    rescue => e
    end

    assert_kind_of RuntimeError, e
    assert_equal 'hi', e.message

    machine.snooze # in case the final CQE has not yet arrived
    assert_equal 0, machine.pending_count
  end

  # send twice
  def test_read_each_raising_3
    r, w = IO.pipe

    bgid = machine.setup_buffer_ring(4096, 1024)
    assert_equal 0, bgid

    w << 'foo'
    w << 'bar'

    e = nil
    begin
      machine.read_each(r.fileno, bgid) do |buf|
        raise 'hi'
      end
    rescue => e
    end

    assert_kind_of RuntimeError, e
    assert_equal 'hi', e.message

    machine.snooze # in case the final CQE has not yet arrived
    assert_equal 0, machine.pending_count
  end

  def test_read_each_break
    r, w = IO.pipe

    bgid = machine.setup_buffer_ring(4096, 1024)

    t = Thread.new do
      sleep 0.1
      w << 'foo'
      sleep 0.1
      w.close
    end
    
    bufs = []
    machine.read_each(r.fileno, bgid) do |b|
      bufs << b
      break
    end

    assert_equal ['foo'], bufs
    machine.snooze # in case the final CQE has not yet arrived
    assert_equal 0, machine.pending_count
  ensure
    t&.kill
  end

  def test_read_each_bad_file
    _r, w = IO.pipe
    bgid = machine.setup_buffer_ring(4096, 1024)

    assert_raises(Errno::EBADF) do
      machine.read_each(w.fileno, bgid)
    end
  end
end

class WriteTest < UMBaseTest
  def test_write
    r, w = IO.pipe

    assert_equal 0, machine.pending_count
    machine.write(w.fileno, 'foo')
    assert_equal 0, machine.pending_count
    assert_equal 'foo', r.readpartial(3)

    machine.write(w.fileno, 'bar', 2)
    assert_equal 'ba', r.readpartial(3)
  end

  def test_write_bad_fd
    r, _w = IO.pipe

    assert_equal 0, machine.pending_count
    assert_raises(Errno::EBADF) do
      machine.write(r.fileno, 'foo')
    end
    assert_equal 0, machine.pending_count
  end
end

class Closetest < UMBaseTest
  def test_close
    r, w = IO.pipe
    machine.write(w.fileno, 'foo')
    assert_equal 'foo', r.readpartial(3)

    assert_equal 0, machine.pending_count
    machine.close(w.fileno)
    assert_equal 0, machine.pending_count
    assert_equal '', r.read

    assert_raises(Errno::EBADF) { machine.close(w.fileno) }
  end
end

class AcceptTest < UMBaseTest
  def setup
    super
    @port = assign_port
    @server = TCPServer.open('127.0.0.1', @port)
  end

  def teardown
    @server&.close
    super
  end

  def test_accept
    conn = TCPSocket.new('127.0.0.1', @port)
    
    assert_equal 0, machine.pending_count
    fd = machine.accept(@server.fileno)
    assert_equal 0, machine.pending_count
    assert_kind_of Integer, fd
    assert fd > 0

    machine.write(fd, 'foo')
    buf = conn.readpartial(3)

    assert_equal 'foo', buf
  end
end

class AcceptEachTest < UMBaseTest
  def setup
    super
    @port = assign_port
    @server = TCPServer.open('127.0.0.1', @port)
  end

  def teardown
    @server&.close
    super
  end

  def test_accept_each
    conns = []
    t = Thread.new do
      sleep 0.05
      3.times { conns << TCPSocket.new('127.0.0.1', @port) }
    end

    count = 0
    machine.accept_each(@server.fileno) do |fd|
      count += 1
      break if count == 3
    end

    assert_equal 3, count
    assert_equal 0, machine.pending_count
  ensure
    t&.kill
  end
end

class SocketTest < UMBaseTest
  def test_socket
    assert_equal 0, machine.pending_count
    fd = machine.socket(UM::AF_INET, UM::SOCK_DGRAM, 0, 0);
    assert_equal 0, machine.pending_count
    assert_kind_of Integer, fd
    assert fd > 0

    assert_raises(Errno::EDESTADDRREQ) { machine.write(fd, 'foo') }
  end
end

class ConnectTest < UMBaseTest
  def setup
    super
    @port = assign_port
    @server = TCPServer.open('127.0.0.1', @port)
  end

  def teardown
    @server&.close
    super
  end

  def test_connect
    t = Thread.new do
      conn = @server.accept
      conn.write('foobar')
      sleep
    end

    fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    assert_equal 0, machine.pending_count
    res = machine.connect(fd, '127.0.0.1', @port)
    assert_equal 0, machine.pending_count
    assert_equal 0, res

    buf = +''
    res = machine.read(fd, buf, 42)
    assert_equal 6, res
    assert_equal 'foobar', buf
  ensure
    t&.kill
  end

  def test_connect_with_bad_addr
    fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0);
    assert_equal 0, machine.pending_count
    assert_raises(Errno::ENETUNREACH) { machine.connect(fd, 'a.b.c.d', @port) }
    assert_equal 0, machine.pending_count
  end
end

class SendTest < UMBaseTest
  def setup
    super
    @port = assign_port
    @server = TCPServer.open('127.0.0.1', @port)
  end

  def teardown
    @server&.close
    super
  end

  def test_send
    t = Thread.new do
      conn = @server.accept
      str = conn.readpartial(42)
      conn.write("You said: #{str}")
      sleep
    end

    fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    res = machine.connect(fd, '127.0.0.1', @port)
    assert_equal 0, res

    res = machine.send(fd, 'foobar', 6, 0)
    assert_equal 6, res

    buf = +''
    res = machine.read(fd, buf, 42)
    assert_equal 16, res
    assert_equal 'You said: foobar', buf
  ensure
    t&.kill
  end
end

class RecvTest < UMBaseTest
  def setup
    super
    @port = assign_port
    @server = TCPServer.open('127.0.0.1', @port)
  end

  def teardown
    @server&.close
    super
  end

  def test_recv
    t = Thread.new do
      conn = @server.accept
      conn.write('foobar')
      sleep
    end

    fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    res = machine.connect(fd, '127.0.0.1', @port)
    assert_equal 0, res

    buf = +''
    res = machine.recv(fd, buf, 42, 0)
    assert_equal 6, res
    assert_equal 'foobar', buf
  ensure
    t&.kill
  end
end

class RecvEachTest < UMBaseTest
  def setup
    super
    @port = assign_port
    @server = TCPServer.open('127.0.0.1', @port)
  end

  def teardown
    @server&.close
    super
  end

  def test_recv_each
    t = Thread.new do
      conn = @server.accept
      conn.write('abc')
      sleep 0.01
      conn.write('def')
      sleep 0.01
      conn.write('ghi')
      sleep 0.01
      conn.close
      sleep
    end

    fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    res = machine.connect(fd, '127.0.0.1', @port)
    assert_equal 0, res

    bgid = machine.setup_buffer_ring(4096, 1024)
    assert_equal 0, bgid

    bgid2 = machine.setup_buffer_ring(4096, 1024)
    assert_equal 1, bgid2

    bufs = []

    machine.recv_each(fd, bgid, 0) do |buf|
      bufs << buf
    end
    assert_equal ['abc', 'def', 'ghi'], bufs
  ensure
    t&.kill
  end
end

class BindTest < UMBaseTest
  def setup
    super
    @port = assign_port
  end

  def test_bind
    assert_equal 0, machine.pending_count
    fd = machine.socket(UM::AF_INET, UM::SOCK_DGRAM, 0, 0)
    res = machine.bind(fd, '127.0.0.1', @port)
    assert_equal 0, res
    assert_equal 0, machine.pending_count

    peer = UDPSocket.new
    peer.connect('127.0.0.1', @port)
    peer.send 'foo', 0

    buf = +''
    res = machine.recv(fd, buf, 8192, 0)
    assert_equal 3, res
    assert_equal 'foo', buf
  end

  def test_bind_invalid_args
    assert_equal 0, machine.pending_count

    fd = machine.socket(UM::AF_INET, UM::SOCK_DGRAM, 0, 0)
    assert_raises(Errno::EACCES) { machine.bind(fd, 'foo.bar.baz', 3) }
    assert_raises(Errno::EBADF) { machine.bind(-3, '127.0.01', 1234) }

    assert_equal 0, machine.pending_count
  end
end

class ListenTest < UMBaseTest
  def setup
    super
    @port = assign_port
  end

  def test_listen
    fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    machine.bind(fd, '127.0.0.1', @port)
    res = machine.listen(fd, 5)
    assert_equal 0, res
    assert_equal 0, machine.pending_count

    conn = nil
    t = Thread.new do
      sleep 0.01
      conn = TCPSocket.new('127.0.0.1', @port)
    end

    conn_fd = machine.accept(fd)
    t.join
    assert_kind_of TCPSocket, conn

    machine.send(conn_fd, 'foo', 3, 0)

    buf = conn.readpartial(42)
    assert_equal 'foo', buf
  ensure
    t&.kill
  end
end

class ConstTest < UMBaseTest
  def test_constants
    assert_equal UM::SOCK_STREAM, UM::SOCK_STREAM
  end
end

class GetSetSockOptTest < UMBaseTest
  def test_getsockopt_setsockopt
    fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    reuseaddr = machine.getsockopt(fd, UM::SOL_SOCKET, UM::SO_REUSEADDR)
    assert_equal 0, reuseaddr

    res = machine.setsockopt(fd, UM::SOL_SOCKET, UM::SO_REUSEADDR, true)
    assert_equal 0, res

    reuseaddr = machine.getsockopt(fd, UM::SOL_SOCKET, UM::SO_REUSEADDR)
    assert_equal 1, reuseaddr
  end
end

class SynchronizeTest < UMBaseTest
  def test_synchronize_single
    skip if !machine.respond_to?(:synchronize)

    m = UM::Mutex.new

    buf = []
    machine.synchronize(m) do
      buf << 1
    end
    machine.synchronize(m) do
      buf << 2
    end

    assert_equal [1, 2], buf
    assert_equal 0, machine.pending_count
  end

  def test_synchronize_pair
    skip if !machine.respond_to?(:synchronize)
    m = UM::Mutex.new

    buf = []

    f1 = Fiber.new do
      machine.synchronize(m) do
        buf << 11
        machine.sleep(0.01)
        buf << 12
      end
      buf << 13
      machine.yield
    end

    f2 = Fiber.new do
      machine.synchronize(m) do
        buf << 21
        machine.sleep(0.01)
        buf << 22
      end
      buf << 23
      machine.yield
    end

    machine.schedule(f1, nil)
    machine.schedule(f2, nil)

    machine.sleep(0.03)
    assert_equal [11, 12, 13, 21, 22, 23], buf
    assert_equal 0, machine.pending_count
  end
end

class QueueTest < UMBaseTest
  def test_push_pop_1
    skip if !machine.respond_to?(:synchronize)

    q = UM::Queue.new
    assert_equal 0, q.count
    machine.push(q, :foo)
    machine.push(q, :bar)
    assert_equal 2, q.count

    assert_equal :bar, machine.pop(q)
    assert_equal 1, q.count
    assert_equal :foo, machine.pop(q)
    assert_equal 0, q.count
  end

  def test_push_pop_2
    skip if !machine.respond_to?(:synchronize)

    q = UM::Queue.new
    buf = []

    f1 = Fiber.new do
      buf << [1, machine.pop(q)]
      machine.yield
    end

    machine.schedule(f1, nil)

    f2 = Fiber.new do
      buf << [2, machine.pop(q)]
      machine.yield
    end

    machine.schedule(f2, nil)

    machine.snooze
    assert_equal [], buf

    machine.push(q, :foo)
    assert_equal 1, q.count
    machine.sleep(0.02)
    assert_equal [[1, :foo]], buf

    machine.push(q, :bar)
    assert_equal 1, q.count

    machine.sleep(0.02)
    assert_equal [[1, :foo], [2, :bar]], buf
    assert_equal 0, q.count
  end

  def test_push_pop_3
    skip if !machine.respond_to?(:synchronize)

    q = UM::Queue.new
    buf = []

    machine.push(q, :foo)
    machine.push(q, :bar)
    assert_equal 2, q.count

    f1 = Fiber.new do
      buf << [1, machine.pop(q)]
      machine.yield
    end
    machine.schedule(f1, nil)

    f2 = Fiber.new do
      buf << [2, machine.pop(q)]
      machine.yield
    end
    machine.schedule(f2, nil)

    3.times { machine.snooze }

    assert_equal [[1, :bar], [2, :foo]], buf.sort
    assert_equal 0, q.count
  end

  def test_push_pop_4
    skip if !machine.respond_to?(:synchronize)

    q = UM::Queue.new
    buf = []

    machine.push(q, :foo)
    assert_equal 1, q.count

    f1 = Fiber.new do
      buf << [1, machine.pop(q)]
      machine.yield
    end
    machine.schedule(f1, nil)

    f2 = Fiber.new do
      buf << [2, machine.pop(q)]
      machine.yield
    end
    machine.schedule(f2, nil)

    machine.sleep 0.01

    assert_equal [[1, :foo]], buf
    machine.push(q, :bar)

    machine.sleep 0.01
    assert_equal [[1, :foo], [2, :bar]], buf
  end

  def test_push_shift_1
    skip if !machine.respond_to?(:synchronize)

    q = UM::Queue.new

    machine.push(q, :foo)
    machine.push(q, :bar)
    machine.push(q, :baz)

    assert_equal :foo, machine.shift(q)
    assert_equal :bar, machine.shift(q)
    assert_equal :baz, machine.shift(q)
  end

  def test_shift_shift_1
    skip if !machine.respond_to?(:synchronize)

    q = UM::Queue.new

    machine.unshift(q, :foo)
    machine.unshift(q, :bar)
    machine.unshift(q, :baz)

    assert_equal :baz, machine.shift(q)
    assert_equal :bar, machine.shift(q)
    assert_equal :foo, machine.shift(q)
  end
end
