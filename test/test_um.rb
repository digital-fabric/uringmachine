# frozen_string_literal: true

require_relative 'helper'
require 'socket'

class UringMachineTest < Minitest::Test
  def test_kernel_version
    v = UringMachine.kernel_version
    assert_kind_of Integer, v
    assert_in_range 600..700, v
  end
end

class SpinTest < UMBaseTest
  def test_spin
    x = nil
    f = machine.spin do
      x = :foo
    end

    assert_kind_of Fiber, f
    assert_nil x

    machine.snooze

    assert_equal :foo, x
  end

  def test_spin_with_unhandled_exception
    f = machine.spin { raise 'foo' }
    result = machine.join(f)
    assert_kind_of RuntimeError, result
  end
end

class SnoozeTest < UMBaseTest
  def test_snooze_while_sleeping_fiber
    machine.spin do
      machine.sleep(0.1)
    end

    t0 = monotonic_clock
    machine.snooze
    t1 = monotonic_clock
    assert_in_range 0..0.001, t1 - t0

    t0 = monotonic_clock
    machine.snooze
    t1 = monotonic_clock
    assert_in_range 0..0.001, t1 - t0
  end
end

class ScheduleTest < UMBaseTest
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

  def test_timeout_stress
    skip
    # GC.stress = true
    c = 0
    fs = 100.times.map {
      machine.spin {
        q = UM::Queue.new
        1000.times {
          machine.sleep rand(0.001..0.005)
          begin
            machine.timeout(rand(0.001..0.06), TOError) do
              machine.shift(q)
            end
          rescue => _e
            c += 1
            STDOUT << '*' if c % 1000 == 0
          end
        }
      }
    }
    machine.join(*fs)
  ensure
    GC.stress = false
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

  def test_timeout_with_no_timeout
    _r, w = UM.pipe
    v = machine.timeout(0.1, TOError) { machine.write(w, 'foo') }

    assert_equal 3, v

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

  def test_sleep_forever
    t0 = monotonic_clock
    ret = begin
      machine.timeout(0.03, D) do
        machine.sleep 0
      end
    rescue => e
      e
    end
    t1 = monotonic_clock
    assert_in_range 0.02..0.04, t1 - t0
    assert_kind_of D, ret
  end
end

class PeriodicallyTest < UMBaseTest
  class Cancel < StandardError; end

  def test_periodically
    count = 0
    cancel = 0

    t0 = monotonic_clock
    assert_equal 0, machine.pending_count
    begin
      machine.periodically(0.01) do
        count += 1
        raise Cancel if count >= 5
      end
    rescue Cancel
      cancel = 1
    end
    machine.snooze
    assert_equal 0, machine.pending_count
    t1 = monotonic_clock
    assert_in_range 0.05..0.09, t1 - t0
    assert_equal 5, count
    assert_equal 1, cancel
  end

  def test_periodically_with_timeout
    count = 0
    cancel = 0

    t0 = monotonic_clock
    assert_equal 0, machine.pending_count
    begin
      machine.timeout(0.05, Cancel) do
        machine.periodically(0.01) do
          count += 1
          raise Cancel if count >= 5
        end
      end
    rescue Cancel
      cancel = 1
    end
    20.times { machine.snooze }
    assert_equal 0, machine.pending_count
    t1 = monotonic_clock
    assert_in_range 0.05..0.08, t1 - t0
    assert_in_range 4..6, count
    assert_equal 1, cancel

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

  def test_read_with_string_io
    require 'stringio'

    buffer = +'foo'
    sio = StringIO.new(buffer)

    r, w = IO.pipe
    w << 'bar'

    result = machine.read(r.fileno, buffer, 100, -1)
    assert_equal 3, result
    assert_equal 'foobar', sio.read

    w << 'baz'

    result = machine.read(r.fileno, buffer, 100, -1)
    assert_equal 3, result
    assert_equal 'baz', sio.read
  end

  def test_read_io_buffer
    r, w = UM.pipe
    machine.write(w, 'foobar')

    read_buffer = IO::Buffer.new(3)
    res = machine.read(r, read_buffer, 3)
    assert_equal 3, res
    assert_equal 'foo', read_buffer.get_string(0, 3)

    machine.close(w)

    res = machine.read(r, read_buffer)
    assert_equal 3, res
    assert_equal 'bar', read_buffer.get_string(0, 3)
  end

  def test_read_io_buffer_resize
    r, w = UM.pipe
    machine.write(w, 'foobar')
    machine.close(w)

    read_buffer = IO::Buffer.new(3)
    res = machine.read(r, read_buffer, 6)
    assert_equal 6, res
    assert_equal 6, read_buffer.size
    assert_equal 'foobar', read_buffer.get_string(0, res)

    r, w = UM.pipe
    machine.write(w, 'foobar')
    machine.close(w)

    read_buffer = IO::Buffer.new(3)
    res = machine.read(r, read_buffer, 128, -1)
    assert_equal 6, res
    assert_equal 131, read_buffer.size
    assert_equal 'foobar', read_buffer.get_string(3, res)
  end

  def test_read_invalid_buffer
    r, _w = UM.pipe
    assert_raises(UM::Error) {
      machine.read(r, [])
    }
  end
end

class ReadEachTest < UMBaseTest
  def test_read_each
    skip if UringMachine.kernel_version < 607

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
    skip if UringMachine.kernel_version < 607

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
    skip if UringMachine.kernel_version < 607

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
    skip if UringMachine.kernel_version < 607

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
    skip if UringMachine.kernel_version < 607

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
    skip if UringMachine.kernel_version < 607

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

  def test_write_io_buffer
    r, w = UM.pipe

    msg = 'Hello world'
    write_buffer = IO::Buffer.new(msg.bytesize)
    write_buffer.set_string(msg, 0)

    machine.write(w, write_buffer)
    machine.close(w)

    str = +''
    machine.read(r, str, 8192)
    assert_equal msg, str
  end

  def test_write_io_buffer_with_len
    r, w = UM.pipe
    msg = 'Hello world'
    write_buffer = IO::Buffer.new(msg.bytesize)
    write_buffer.set_string(msg)

    machine.write(w, write_buffer, 5)
    machine.close(w)

    str = +''
    machine.read(r, str, 8192)
    assert_equal 'Hello', str

    r, w = UM.pipe
    msg = 'Hello world'
    write_buffer = IO::Buffer.new(msg.bytesize)
    write_buffer.set_string(msg)

    machine.write(w, write_buffer, -1)
    machine.close(w)

    str = +''
    machine.read(r, str, 8192)
    assert_equal 'Hello world', str
  end

  def test_write_invalid_buffer
    _r, w = UM.pipe
    assert_raises(UM::Error) {
      machine.write(w, [])
    }
  end
end

class WriteAsyncTest < UMBaseTest
  def test_write_async
    r, w = IO.pipe

    assert_equal 0, machine.pending_count
    machine.write_async(w.fileno, 'foo')
    assert_equal 1, machine.pending_count

    machine.snooze
    assert_equal 0, machine.pending_count
    assert_equal 'foo', r.readpartial(3)
  end

  def test_write_async_dynamic_string
    r, w = IO.pipe

    assert_equal 0, machine.pending_count
    str = "foo#{123}#{'bar' * 48}"
    len = str.bytesize
    machine.write_async(w.fileno, str)
    str = nil
    GC.start
    assert_equal 1, machine.pending_count

    machine.snooze
    assert_equal 0, machine.pending_count
    assert_equal "foo#{123}#{'bar' * 48}", r.readpartial(len)
  end

  def test_write_async_bad_fd
    r, _w = IO.pipe

    assert_equal 0, machine.pending_count
    machine.write_async(r.fileno, 'foo')
    assert_equal 1, machine.pending_count
    machine.snooze
    assert_equal 0, machine.pending_count
  end

  def test_write_async_io_buffer
    r, w = UM.pipe

    msg = 'Hello world'
    write_buffer = IO::Buffer.new(msg.bytesize)
    write_buffer.set_string(msg)

    machine.write_async(w, write_buffer)
    3.times { machine.snooze }
    machine.close(w)

    str = +''
    machine.read(r, str, 8192)
    assert_equal msg, str
  end

  def test_write_async_invalid_buffer
    _r, w = UM.pipe

    assert_raises(UM::Error) { machine.write_async(w, []) }
  end
end

class CloseTest < UMBaseTest
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

  def test_close_bad_fd
    _r, w = IO.pipe
    machine.close(w.fileno)

    assert_raises(Errno::EBADF) { machine.close(w.fileno) }
  end
end

class CloseAsyncTest < UMBaseTest
  def test_close_async
    r, w = IO.pipe
    machine.write(w.fileno, 'foo')
    assert_equal 'foo', r.readpartial(3)

    assert_equal 0, machine.pending_count
    machine.close_async(w.fileno)
    assert_equal 1, machine.pending_count
    machine.snooze
    assert_equal 0, machine.pending_count
    assert_equal '', r.read
  end
end

class ShutdownTest < UMBaseTest
  def test_shutdown
    c_fd, s_fd = make_socket_pair
    res = @machine.send(c_fd, 'abc', 3, 0)
    assert_equal 3, res

    buf = +''
    res = @machine.recv(s_fd, buf, 256, 0)
    assert_equal 3, res
    assert_equal 'abc', buf

    res = @machine.shutdown(c_fd, UM::SHUT_WR)
    assert_equal 0, res

    assert_raises(Errno::EPIPE) { @machine.send(c_fd, 'abc', 3, 0) }

    res = @machine.shutdown(s_fd, UM::SHUT_RD)
    assert_equal 0, res

    res = @machine.recv(s_fd, buf, 256, 0)
    assert_equal 0, res

    res = @machine.shutdown(c_fd, UM::SHUT_RDWR)
    assert_equal 0, res

    assert_raises(Errno::EINVAL) { @machine.shutdown(c_fd, -9999) }
  ensure
    @machine.close(c_fd)
    @machine.close(s_fd)
  end
end

class ShutdownAsyncTest < UMBaseTest
  def test_shutdown_async
    c_fd, s_fd = make_socket_pair
    res = @machine.send(c_fd, 'abc', 3, 0)
    assert_equal 3, res

    buf = +''
    res = @machine.recv(s_fd, buf, 256, 0)
    assert_equal 3, res
    assert_equal 'abc', buf

    res = @machine.shutdown_async(c_fd, UM::SHUT_WR)
    assert_equal c_fd, res

    machine.sleep(0.01)
    assert_raises(Errno::EPIPE) { @machine.send(c_fd, 'abc', 3, 0) }

    res = @machine.shutdown_async(s_fd, UM::SHUT_RD)
    assert_equal s_fd, res

    res = @machine.recv(s_fd, buf, 256, 0)
    assert_equal 0, res

    res = @machine.shutdown_async(c_fd, UM::SHUT_RDWR)
    assert_equal c_fd, res
  ensure
    @machine.close(c_fd)
    @machine.close(s_fd)
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

  def test_accept_each_interrupted
    count = 0
    terminated = nil
    f = @machine.spin do
      machine.accept_each(@server.fileno) do |fd|
        count += 1
        break if count == 3
      end
    rescue UM::Terminate
      terminated = true
    end

    s = TCPSocket.new('127.0.0.1', @port)
    @machine.sleep(0.01)

    assert_equal 1, count
    refute terminated

    @machine.schedule(f, UM::Terminate.new)
    @machine.sleep(0.01)

    assert f.done?
    assert terminated
  ensure
    s.close
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


  def test_send_io_buffer
    @port = assign_port
    @server = TCPServer.open('127.0.0.1', @port)

    t = Thread.new do
      conn = @server.accept
      str = conn.readpartial(42)
      conn.write("You said: #{str} (#{str.bytesize})")
      sleep
    end

    fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    res = machine.connect(fd, '127.0.0.1', @port)
    assert_equal 0, res

    buffer = IO::Buffer.new(6)
    buffer.set_string('foobar')
    res = machine.send(fd, buffer, 6, 0)
    assert_equal 6, res

    buf = +''
    res = machine.read(fd, buf, 42)
    assert_equal 20, res
    assert_equal 'You said: foobar (6)', buf
  ensure
    t&.kill
    @server&.close
  end

  def test_send_io_buffer_negative_len
    t = Thread.new do
      conn = @server.accept
      str = conn.readpartial(42)
      conn.write("You said: #{str} (#{str.bytesize})")
      sleep
    end

    fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    res = machine.connect(fd, '127.0.0.1', @port)
    assert_equal 0, res

    buffer = IO::Buffer.new(6)
    buffer.set_string('foobar')
    res = machine.send(fd, buffer, -1, 0)
    assert_equal 6, res

    buf = +''
    res = machine.read(fd, buf, 42)
    assert_equal 20, res
    assert_equal 'You said: foobar (6)', buf
  ensure
    t&.kill
  end

  def test_send_invalid_buffer
    t = Thread.new do
      conn = @server.accept
      str = conn.readpartial(42)
      conn.write("You said: #{str} (#{str.bytesize})")
      sleep
    end

    fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    res = machine.connect(fd, '127.0.0.1', @port)
    assert_equal 0, res

    assert_raises(UM::Error) { machine.send(fd, [], -1, 0) }
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

  def test_recv_io_buffer
    t = Thread.new do
      conn = @server.accept
      conn.write('foobar')
      sleep
    end

    fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    res = machine.connect(fd, '127.0.0.1', @port)
    assert_equal 0, res

    buf = IO::Buffer.new(12)
    res = machine.recv(fd, buf, 12, 0)
    assert_equal 6, res
    assert_equal 'foobar', buf.get_string(0, 6)
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

  def test_synchronize_multi
    mutex = UM::Mutex.new
    buf = []
    fibers = (1..8).map { |i|
      machine.spin do
        machine.synchronize(mutex) do
          buf << (i * 10) + 1
          machine.sleep(0.01)
          buf << (i * 10) + 2
        end
        machine.snooze
        buf << (i * 10) + 3
      end
    }

    machine.join(*fibers)
    assert_equal [11, 12, 21, 13, 22, 31, 23, 32, 41, 33, 42, 51, 43, 52, 61, 53, 62, 71, 63, 72, 81, 73, 82, 83], buf
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

    machine.spin do
      buf << [1, machine.pop(q)]
    end

    machine.spin do
      buf << [2, machine.pop(q)]
    end

    machine.snooze
    assert_equal [], buf
    assert_equal 2, machine.pending_count

    machine.push(q, :foo)
    assert_equal 1, q.count
    machine.snooze
    assert_equal 1, machine.pending_count
    assert_equal [[1, :foo]], buf

    machine.push(q, :bar)
    assert_equal 1, q.count

    machine.snooze
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

    machine.snooze

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

    machine.snooze
    assert_equal [[1, :foo]], buf

    machine.push(q, :bar)
    machine.snooze
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

  def test_cross_thread_push_shift
    q = UM::Queue.new

    t1 = Thread.new {
      m = UM.new
      3.times { m.push(q, it) }
    }

    items = []

    t2 = Thread.new {
      m = UM.new
      3.times {
        i = m.shift(q)
        items << i
        m.sleep(0.01)
      }
    }

    [t1, t2].each(&:join)

    assert_equal [0, 1, 2], items
  end
end

class OpenTest < UMBaseTest
  PATH = '/tmp/um_open_test'

  def setup
    super
    FileUtils.rm(PATH, force: true)
  end

  def test_open
    fd = machine.open(PATH, UM::O_CREAT | UM::O_WRONLY)
    assert_kind_of Integer, fd
    assert File.file?(PATH)

    machine.write(fd, 'foo')
    machine.close(fd)

    assert_equal 'foo', IO.read(PATH)
  end

  def test_open_with_block
    res = machine.open(PATH, UM::O_CREAT | UM::O_WRONLY) do |fd|
      machine.write(fd, 'bar')
      fd
    end

    assert_kind_of Integer, res
    assert_raises(Errno::EBADF) { machine.close(res) }
    assert_equal 'bar', IO.read(PATH)
  end

  def test_open_bad_arg
    assert_raises(Errno::ENOENT) { machine.open(PATH, UM::O_RDONLY) }
    assert_raises(Errno::ENOENT) { machine.open(PATH, UM::O_RDONLY) {} }
  end
end

class PipeTest < UMBaseTest
  def test_pipe
    rfd, wfd = UM.pipe
    ret = machine.write(wfd, 'foo')
    assert_equal 3, ret

    ret = machine.close(wfd)
    assert_equal wfd, ret

    buf = +''
    ret = machine.read(rfd, buf, 8192)

    assert_equal 3, ret
    assert_equal 'foo', buf

    ret = machine.close(rfd)
    assert_equal rfd, ret
  end
end

class PidfdTest < UMBaseTest
  def test_pidfd_open
    pid = fork { exit 13 }
    fd = UM.pidfd_open(pid)
    assert_kind_of Integer, fd

    pid2, status = machine.waitid(P_PIDFD, fd, UM::WEXITED)
    assert_equal pid, pid2
    assert_equal 13, status
  ensure
    machine.close(fd) rescue nil
    Process.wait(pid) rescue nil
  end

  def test_pidfd_open_invalid_pid
    assert_raises(SystemCallError) { UM.pidfd_open(Process.pid + 1) }
  end

  def test_pidfd_send_signal
    fd = UM.pidfd_open(Process.pid)
    buf = []
    trap('SIGUSR1') { buf << :SIGUSR1 }

    ret = UM.pidfd_send_signal(fd, UM::SIGUSR1)
    assert_equal fd, ret
    sleep 0.01

    assert_equal [:SIGUSR1], buf
  ensure
    machine.close(fd) rescue nil
  end

  def test_pidfd_send_signal_test
    fd = UM.pidfd_open(Process.pid)
    buf = []
    trap('SIGUSR1') { buf << :SIGUSR1 }

    ret = UM.pidfd_send_signal(fd, 0)
    assert_equal fd, ret
    sleep 0.01
    assert_equal [], buf

    pid = fork { exit 13 }
    fd2 = UM.pidfd_open(pid)
    assert_kind_of Integer, fd2

    Process.wait(pid)
    assert_raises(SystemCallError) { UM.pidfd_send_signal(fd2, 0) }
  ensure
    machine.close(fd) rescue nil
    machine.close(fd2) rescue nil
    Process.wait(pid) rescue nil
  end
end

class PollTest < UMBaseTest
  def test_poll
    rfd, wfd = UM.pipe

    events = []
    machine.spin do
      events << :pre
      events << machine.poll(rfd, UM::POLLIN)
      events << :post
    end

    machine.snooze
    assert_equal [:pre], events

    machine.write(wfd, 'foo')
    machine.snooze
    assert_equal [:pre, UM::POLLIN, :post], events

    ret = machine.poll(wfd, UM::POLLOUT)
    assert_equal UM::POLLOUT, ret

    machine.close(rfd)
    ret = machine.poll(wfd, UM::POLLOUT | UM::POLLERR)
    assert_equal UM::POLLOUT | UM::POLLERR, ret
  end

  def test_poll_bad_fd
    assert_raises(Errno::EBADF) { machine.poll(9876, POLLIN) }
  end
end

class WaitidTest < UMBaseTest
  def test_waitid
    msg = 'hello from child'
    _rfd, wfd = UM.pipe
    child_pid = fork do
      m = UM.new
      m.write(wfd, msg)
      m.close(wfd)
      exit 42
    end

    pid, status = machine.waitid(UM::P_PID, child_pid, UM::WEXITED)
    assert_equal child_pid, pid
    assert_equal 42, status
  ensure
    Process.wait(child_pid) rescue nil
  end

  def test_waitid_invalid_pid
    assert_raises(Errno::ECHILD) {
      machine.waitid(UM::P_PID, Process.pid + 1, UM::WEXITED)
    }
  end

  def test_waitid_invalid_idtype
    assert_raises(Errno::EINVAL) {
      machine.waitid(1234, 0, UM::WEXITED)
    }
  end

  def test_waitid_invalid_options
    assert_raises(Errno::EINVAL) {
      machine.waitid(P_ALL, 0, 1234)
    }
  end

  def test_waitid_P_ALL
    msg = 'hello from child'
    _rfd, wfd = UM.pipe
    child_pid = fork do
      m = UM.new
      m.write(wfd, msg)
      m.close(wfd)
      exit 42
    end

    pid, status = machine.waitid(UM::P_ALL, 0, UM::WEXITED)
    assert_equal child_pid, pid
    assert_equal 42, status
  ensure
    Process.wait(child_pid) rescue nil
  end

  def test_waitid_P_PGID
    msg = 'hello from child'
    _rfd, wfd = UM.pipe
    child_pid = fork do
      m = UM.new
      m.write(wfd, msg)
      m.close(wfd)
      exit 42
    end

    pid, status = machine.waitid(UM::P_PGID, Process.getpgrp, UM::WEXITED)
    assert_equal child_pid, pid
    assert_equal 42, status
  ensure
    Process.wait(child_pid) rescue nil
  end

  def test_waitid_status
    skip if !machine.respond_to?(:waitid_status)

    msg = 'hello from child'
    _rfd, wfd = UM.pipe
    pid = fork do
      m = UM.new
      m.write(wfd, msg)
      m.close(wfd)
      exit 42
    end

    status = machine.waitid_status(UM::P_PID, pid, UM::WEXITED)
    assert_kind_of Process::Status, status
    assert_equal pid, status.pid
    assert_equal 42, status.exitstatus
  ensure
    Process.wait(pid) rescue nil
  end

  def test_waitid_status_invalid_pid
    skip if !machine.respond_to?(:waitid_status)

    assert_raises(Errno::ECHILD) {
      machine.waitid_status(UM::P_PID, Process.pid + 1, UM::WEXITED)
    }
  end

  def test_waitid_status_invalid_idtype
    skip if !machine.respond_to?(:waitid_status)

    assert_raises(Errno::EINVAL) {
      machine.waitid_status(1234, 0, UM::WEXITED)
    }
  end

  def test_waitid_status_invalid_options
    skip if !machine.respond_to?(:waitid_status)

    assert_raises(Errno::EINVAL) {
      machine.waitid_status(P_ALL, 0, 1234)
    }
  end

  def test_waitid_status_P_ALL
    skip if !machine.respond_to?(:waitid_status)

    msg = 'hello from child'
    _rfd, wfd = UM.pipe
    pid = fork do
      m = UM.new
      m.write(wfd, msg)
      m.close(wfd)
      exit 42
    end

    status = machine.waitid_status(UM::P_ALL, 0, UM::WEXITED)
    assert_kind_of Process::Status, status
    assert_equal pid, status.pid
    assert_equal 42, status.exitstatus
  ensure
    Process.wait(pid) rescue nil
  end

  def test_waitid_status_P_PGID
    skip if !machine.respond_to?(:waitid_status)

    msg = 'hello from child'
    _rfd, wfd = UM.pipe
    pid = fork do
      m = UM.new
      m.write(wfd, msg)
      m.close(wfd)
      exit 42
    end

    status = machine.waitid_status(UM::P_PGID, Process.getpgrp, UM::WEXITED)
    assert_kind_of Process::Status, status
    assert_equal pid, status.pid
    assert_equal 42, status.exitstatus
  ensure
    Process.wait(pid) rescue nil
  end
end

class StatxTest < UMBaseTest
  def test_statx
    io = File.open(__FILE__, 'r')
    ustat = machine.statx(io.fileno, nil, UM::AT_EMPTY_PATH, UM::STATX_ALL)
    rstat = File.stat(__FILE__)

    assert_equal rstat.dev,         ustat[:dev]
    assert_equal rstat.ino,         ustat[:ino]
    assert_equal rstat.mode,        ustat[:mode]
    assert_equal rstat.nlink,       ustat[:nlink]
    assert_equal rstat.uid,         ustat[:uid]
    assert_equal rstat.gid,         ustat[:gid]
    assert_equal rstat.rdev,        ustat[:rdev]
    assert_equal rstat.size,        ustat[:size]
    assert_equal rstat.blksize,     ustat[:blksize]
    assert_equal rstat.blocks,      ustat[:blocks]
    assert_equal rstat.atime.to_i,  ustat[:atime].to_i
    assert_equal rstat.ctime.to_i,  ustat[:ctime].to_i
    assert_equal rstat.mtime.to_i,  ustat[:mtime].to_i

    ustat2 = machine.statx(UM::AT_FDCWD, __FILE__, 0, UM::STATX_ALL)
    assert_equal rstat.dev,         ustat2[:dev]
    assert_equal rstat.ino,         ustat2[:ino]
    assert_equal rstat.mode,        ustat2[:mode]
    assert_equal rstat.nlink,       ustat2[:nlink]
    assert_equal rstat.uid,         ustat2[:uid]
    assert_equal rstat.gid,         ustat2[:gid]
    assert_equal rstat.rdev,        ustat2[:rdev]
    assert_equal rstat.size,        ustat2[:size]
    assert_equal rstat.blksize,     ustat2[:blksize]
    assert_equal rstat.blocks,      ustat2[:blocks]
    assert_equal rstat.atime.to_i,  ustat2[:atime].to_i
    assert_equal rstat.ctime.to_i,  ustat2[:ctime].to_i
    assert_equal rstat.mtime.to_i,  ustat2[:mtime].to_i
  ensure
    io.close
  end

  def test_statx_mask
    fd = @machine.open(__FILE__, UM::O_RDONLY)
    ustat = machine.statx(fd, nil, UM::AT_EMPTY_PATH, UM::STATX_MTIME | UM::STATX_SIZE)
    rstat = File.stat(__FILE__)

    assert_equal rstat.size,        ustat[:size]
    assert_equal rstat.mtime.to_i,  ustat[:mtime].to_i
  ensure
    @machine.close_async(fd)
  end

  def test_statx_bad_path
    assert_raises(Errno::ENOENT) { machine.statx(UM::AT_FDCWD, 'foobar', 0, UM::STATX_ALL) }
  end
end

class ForkTest < UMBaseTest
  def test_fork
    parent_rfd, child_wfd = UM.pipe
    child_rfd, parent_wfd = UM.pipe

    child_pid = fork do
      # we cannot use the same machine after fork
      m = UM.new
      buf = +''
      m.read(child_rfd, buf, 8192)
      m.write(child_wfd, buf, buf.bytesize)
      m.close(child_wfd)
    rescue Exception => e
      puts 'c' * 40
      p e
      puts e.backtrace.join("\n")
    end

    ret = machine.write(parent_wfd, 'foo')
    assert_equal 3, ret

    ret = machine.close(parent_wfd)
    assert_equal parent_wfd, ret

    buf = +''
    ret = machine.read(parent_rfd, buf, 8192)

    assert_equal 3, ret
    assert_equal 'foo', buf
  ensure
    Process.wait(child_pid) rescue nil
  end
end

class SendBundleTest < UMBaseTest
  def setup
    super
    @client_fd, @server_fd = make_socket_pair
  end

  def test_send_bundle_splat
    skip if UringMachine.kernel_version < 610

    bgid = machine.setup_buffer_ring(0, 8)
    assert_equal 0, bgid

    strs = ['foo', 'bar', 'bazzzzz']
    len = strs.inject(0) { |len, s| len + s.bytesize }

    ret = machine.send_bundle(@client_fd, bgid, *strs)
    assert_equal len, ret

    buf = +''
    ret = machine.recv(@server_fd, buf, 8192, 0)
    assert_equal len, ret
    assert_equal strs.join, buf
  end

  def test_send_bundle_array
    skip if UringMachine.kernel_version < 610

    bgid = machine.setup_buffer_ring(0, 8)
    assert_equal 0, bgid

    strs = ['foo', 'bar', 'bazzzzz']
    len = strs.inject(0) { |len, s| len + s.bytesize }

    ret = machine.send_bundle(@client_fd, bgid, strs)
    assert_equal len, ret

    buf = +''
    ret = machine.recv(@server_fd, buf, 8192, 0)
    assert_equal len, ret
    assert_equal strs.join, buf
  end

  def test_send_bundle_non_strings
    skip if UringMachine.kernel_version < 610

    bgid = machine.setup_buffer_ring(0, 8)
    assert_equal 0, bgid

    strs = [42, 'bar', false]
    len = strs.inject(0) { |len, s| len + s.to_s.bytesize }

    ret = machine.send_bundle(@client_fd, bgid, strs)
    assert_equal len, ret

    buf = +''
    ret = machine.recv(@server_fd, buf, 8192, 0)
    assert_equal len, ret
    assert_equal strs.map(&:to_s).join, buf
  end
end

class NonBlockTest < UMBaseTest
  def test_io_nonblock?
    assert_equal false, UM.io_nonblock?(STDIN)
  end

  def test_io_set_nonblock
    r, _w = IO.pipe
    assert_equal true, UM.io_nonblock?(r)

    UM.io_set_nonblock(r, false)
    assert_equal false, UM.io_nonblock?(r)

    UM.io_set_nonblock(r, true)
    assert_equal true, UM.io_nonblock?(r)
  end
end
