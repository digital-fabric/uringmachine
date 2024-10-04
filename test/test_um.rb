# frozen_string_literal: true

require_relative 'helper'
require 'socket'

class SchedulingTest < UMBaseTest
  def test_schedule_and_yield
    buf = []
    cur = Fiber.current
    f = Fiber.new do |x|
      buf << [21, x]
      machine.schedule(cur, 21)
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

  def test_interrupt
    cur = Fiber.current
    e = CustomError.new
    f = Fiber.new do
      machine.interrupt(cur, e)
      assert_equal 2, machine.pending_count
      machine.yield
    end
    
    machine.schedule(f, nil)
    t0 = monotonic_clock

    # the call to schedule means an op is checked out
    assert_equal 1, machine.pending_count
    begin
      machine.sleep(1)
    rescue Exception => e2
    end
    # the sleep op has been cancelled, but we still need to process the
    # cancellation. Calling snooze should take care of that.
    assert_equal 1, machine.pending_count
    machine.snooze

    # CQE should have been received, and the op checked in
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

    # at this point, the sleep cancelled CQE should not yet have been received.
    # So we still have a pending operation. Snooze should have let the CQE be
    # received.
    assert_equal 1, machine.pending_count
    machine.snooze
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
    machine.snooze
    assert_equal 0, machine.pending_count

    assert_kind_of RuntimeError, e
    assert_equal 'hi', e.message
  end

  def test_timeout_with_nothing_blocking
    v = machine.timeout(0.1, TOError) { 42 }

    assert_equal 42, v

    assert_equal 1, machine.pending_count
    machine.snooze
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

    assert_kind_of TO2Error, e
    assert_equal [3], buf
  end
end

class SleepTest < UMBaseTest
  def test_sleep
    t0 = monotonic_clock
    res = machine.sleep(0.1)
    t1 = monotonic_clock
    assert_in_range 0.09..0.13, t1 - t0
    assert_equal 0.1, res
  end
end

class ReadTest < UMBaseTest
  def test_read
    r, w = IO.pipe
    w << 'foobar'

    buf = +''
    res = machine.read(r.fileno, buf, 3)
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
      machine.snooze
      w << 'bar'
      machine.snooze
      w << 'baz'
      machine.snooze
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

    # since the write fd is still open, the read_each impl is supposed to cancel
    # the op, which is done asynchronously.
    assert_equal 1, machine.pending_count
    machine.snooze
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

    # since the write fd is still open, the read_each impl is supposed to cancel
    # the op, which is done asynchronously.
    assert_equal 1, machine.pending_count
    machine.snooze
    assert_equal 0, machine.pending_count
  end
end

class WriteTest < UMBaseTest
  def test_write
    r, w = IO.pipe

    machine.write(w.fileno, 'foo')
    assert_equal 'foo', r.readpartial(3)

    machine.write(w.fileno, 'bar', 2)
    assert_equal 'ba', r.readpartial(3)
  end

  def test_write_bad_fd
    r, _w = IO.pipe

    assert_raises(Errno::EBADF) do
      machine.write(r.fileno, 'foo')
    end
  end
end

class AcceptTest < UMBaseTest
  def setup
    super
    @port = 9000 + rand(1000)
    @server = TCPServer.open('127.0.0.1', @port)
  end

  def teardown
    @server&.close
    super
  end

  def test_accept
    conn = TCPSocket.new('127.0.0.1', @port)
    
    fd = machine.accept(@server.fileno)
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
    @port = 9000 + rand(1000)
    @server = TCPServer.open('127.0.0.1', @port)
  end

  def teardown
    @server&.close
    super
  end

  def test_accept_each
    conns = 3.times.map { TCPSocket.new('127.0.0.1', @port) }

    count = 0
    machine.accept_each(@server.fileno) do |fd|
      machine.write(fd, (count += 1).to_s)
      break if count == 3
    end

    assert_equal 3, count
    assert_equal 1, machine.pending_count
    machine.snooze
    assert_equal 0, machine.pending_count

    assert_equal '1', conns[0].readpartial(3)
    assert_equal '2', conns[1].readpartial(3)
    assert_equal '3', conns[2].readpartial(3)
  end
end
