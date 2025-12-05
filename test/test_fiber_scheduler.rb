# frozen_string_literal: true

require_relative 'helper'
require 'uringmachine/fiber_scheduler'
require 'securerandom'
require 'socket'

class MethodCallAuditor
  attr_reader :calls
  
  def initialize(target)
    @target = target
    @calls = []
  end

  def respond_to?(sym, include_all = false) = @target.respond_to?(sym, include_all)
  
  def method_missing(sym, *args, &block)
    res = @target.send(sym, *args, &block)
    @calls << ({ sym:, args:, res:})
    res
  rescue => e
    @calls << ({ sym:, args:, res: e})
    raise
  end

  def last_call
    calls.last
  end
end

class FiberSchedulerTest < UMBaseTest
  def setup
    super
    @raw_scheduler = UM::FiberScheduler.new(@machine)
    @scheduler = MethodCallAuditor.new(@raw_scheduler)
    Fiber.set_scheduler(@scheduler)
  end

  def teardown
    Fiber.set_scheduler(nil)
    GC.start
  end

  def test_fiber_scheduler_initialize_without_machine
    s = UM::FiberScheduler.new
    assert_kind_of UringMachine, s.machine
  end

  def test_fiber_scheduler_spinning
    f1 = Fiber.schedule do
      sleep 0.001
    end

    f2 = Fiber.schedule do
      sleep 0.001
    end

    assert_kind_of Fiber, f1
    assert_kind_of Fiber, f2

    assert_equal 2, @scheduler.calls.size
    assert_equal [:fiber] * 2, @scheduler.calls.map { it[:sym] }
    assert_equal 2, @scheduler.fiber_map.size

    # close scheduler
    Fiber.set_scheduler nil
    assert_equal :scheduler_close, @scheduler.last_call[:sym]
    GC.start
    assert_equal 0, @scheduler.fiber_map.size
  end

  def test_fiber_scheduler_io_read_io_write
    i, o = IO.pipe
    buffer = []

    f1 = Fiber.schedule do
      sleep 0.01
      o.write 'foo'
      buffer << :f1
    end

    f2 = Fiber.schedule do
      sleep 0.02
      o.write 'bar'
      buffer << :f2
      o.close
    end

    f3 = Fiber.schedule do
      str = i.read
      buffer << str
    end

    @scheduler.join
    assert_equal [true] * 3, [f1, f2, f3].map(&:done?)
    assert_equal [:f1, :f2, 'foobar'], buffer

    assert_equal({
      fiber: 3,
      kernel_sleep: 2,
      io_write: 2,
      io_read: 3,
      blocking_operation_wait: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  ensure
    i.close rescue nil
    o.close rescue nil
  end

  def test_io_read_with_timeout
    i, o = IO.pipe
    i.timeout = 0.01
    buf = []

    Fiber.schedule do
      buf << i.read
    rescue Timeout::Error
      buf << :timeout
    end
    @scheduler.join
    assert_equal [:timeout], buf
    
    assert_equal({
      fiber: 1,
      io_read: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_io_write_with_timeout
    i, o = IO.pipe
    o << ('*' * (1 << 16))
    o.timeout = 0.01
    
    buf = []

    Fiber.schedule do
      buf << o.write('!')
    rescue Timeout::Error
      buf << :timeout
    end
    @scheduler.join
    assert_equal [:timeout], buf
    
    assert_equal({
      fiber: 1,
      io_write: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_fiber_io_pread
    fn = "/tmp/#{SecureRandom.hex}"
    IO.write(fn, 'foobar')

    buf = nil
    Fiber.schedule do
      File.open(fn, 'r') do |f|     
        buf = f.pread(3, 2)
      end
    rescue => e
      buf = e
    end

    @scheduler.join
    assert_equal 'oba', buf
    assert_equal({
      fiber: 1,
      blocking_operation_wait: 1,
      io_pread: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_fiber_io_pwrite
    skip

    fn = "/tmp/#{SecureRandom.hex}"
    IO.write(fn, 'foobar')

    res = nil
    Fiber.schedule do
      File.open(fn, 'w') do |f|
        # f.write('baz')
        res = f.pwrite('baz', 2)
      end
    end

    @scheduler.join
    assert_equal 3, buf
    
    assert_equal 'fobazr', IO.read(fn)
    assert_equal({
      fiber: 1,
      blocking_operation_wait: 1,
      io_pread: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_fiber_scheduler_sleep
    t0 = monotonic_clock
    assert_equal 0, machine.pending_count
    Fiber.schedule do
      sleep(0.01)
    end
    Fiber.schedule do
      sleep(0.02)
    end
    assert_equal 2, machine.pending_count
    @scheduler.join
    t1 = monotonic_clock
    assert_in_range 0.02..0.025, t1 - t0

    assert_equal({
      fiber: 2,
      kernel_sleep: 2,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_fiber_scheduler_lock
    mutex = Mutex.new
    buffer = []
    t0 = monotonic_clock
    Fiber.schedule do
      10.times { sleep 0.001; buffer << it }
    end
    Fiber.schedule do
      mutex.synchronize { sleep(0.005) }
    end
    Fiber.schedule do
      mutex.synchronize { sleep(0.005) }
    end
    @scheduler.join
    t1 = monotonic_clock
    assert_in_range 0.01..0.020, t1 - t0
    assert_equal({
      fiber: 3,
      kernel_sleep: 12,
      block: 1,
      unblock: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_fiber_scheduler_process_wait
    skip if !@scheduler.respond_to?(:process_wait)

    child_pid = nil
    status = nil
    f1 = Fiber.schedule do
      child_pid = fork {
        Fiber.scheduler.process_fork
        Fiber.set_scheduler nil
        sleep(0.01);
        exit! 42
      }
      status = Process::Status.wait(child_pid)
    rescue => e
      p e
    end
    @scheduler.join(f1)
    assert_kind_of Process::Status, status
    assert_equal child_pid, status.pid
    assert_equal 42, status.exitstatus
    assert_equal({
      fiber: 1,
      process_wait: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  ensure
    if child_pid
      Process.wait(child_pid) rescue nil
    end
  end

  # Currently the fiber scheduler doesn't have hooks for send/recv. The only
  # hook that will be invoked is `io_wait`.
  def test_fiber_scheduler_sockets
    s1, s2 = UNIXSocket.pair(:STREAM)

    buf = +''
    sent = nil

    assert_equal 0, machine.total_op_count
    Fiber.schedule do
      buf = s1.recv(12)
    end
    Fiber.schedule do
      sent = s2.send('foobar', 0)
    end

    # In Ruby, sockets are by default non-blocking. The recv will cause io_wait
    # to be invoked, the send should get through without needing to poll.
    assert_equal 1, machine.total_op_count
    @scheduler.join
    
    assert_equal 6, sent
    assert_equal 'foobar', buf
    assert_equal({
      fiber: 2,
      io_wait: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  ensure
    s1.close rescue nil
    s2.close rescue nil
  end

  def test_fiber_scheduler_IO_write_IO_read
    fn = "/tmp/#{SecureRandom.hex}"
    Fiber.schedule do
      IO.write(fn, 'foobar')
    end
    assert_equal 1, machine.total_op_count

    buf = nil
    Fiber.schedule do
      buf = IO.read(fn)
    end
    assert_equal 2, machine.total_op_count
    
    @scheduler.join
    assert_equal 'foobar', buf
    assert_equal({
      fiber: 2,
      blocking_operation_wait: 3,
      io_read: 2,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_fiber_scheduler_file_io
    fn = "/tmp/#{SecureRandom.hex}"
    Fiber.schedule do
      File.open(fn, 'w') { it.write 'foobar' }
    end
    assert_equal 1, machine.total_op_count

    buf = nil
    Fiber.schedule do
      File.open(fn, 'r') { buf = it.read }
    end
    assert_equal 2, machine.total_op_count
    @scheduler.join
    assert_equal 'foobar', buf
    assert_equal({
      fiber: 2,
      blocking_operation_wait: 3,
      io_read: 2,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_fiber_scheduler_mutex
    mutex = Mutex.new

    buf = []
    Fiber.schedule do
      buf << 11
      mutex.synchronize {
        buf << [12, machine.total_op_count]
        sleep 0.01
        buf << [13, machine.total_op_count]
      }
      buf << 14
    end
    assert_equal 1, machine.total_op_count

    Fiber.schedule do
      buf << 21
      mutex.synchronize {
        buf << [22, machine.total_op_count]
        sleep 0.01
        buf << [23, machine.total_op_count]
      }
      buf << 24
    end
    assert_equal 1, machine.total_op_count

    @scheduler.join
    assert_equal [11, [12, 0], 21, [13, 2], 14, [22, 2], [23, 4], 24], buf
    assert_equal({
      fiber: 2,
      kernel_sleep: 2,
      block: 1,
      unblock: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_fiber_scheduler_queue_shift
    queue = Queue.new

    buf = []
    Fiber.schedule do
      buf << [11, machine.total_op_count]
      buf << queue.shift
      buf << [12, machine.total_op_count]
    end
    Fiber.schedule do
      buf << [21, machine.total_op_count]
      queue << :foo
      buf << [22, machine.total_op_count]
    end
    assert_equal 0, machine.total_op_count
    @scheduler.join

    assert_equal [[11, 0], [21, 0], [22, 0], :foo, [12, 1]], buf
    assert_equal({
      fiber: 2,
      block: 1,
      unblock: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_fiber_scheduler_queue_shift_with_timeout
    queue = Queue.new

    buf = []
    Fiber.schedule do
      buf << [11, machine.total_op_count]
      buf << queue.shift(timeout: 0.01)
      buf << [12, machine.total_op_count]
    end
    Fiber.schedule do
      buf << [21, machine.total_op_count]
    end
    assert_equal 1, machine.total_op_count
    @scheduler.join

    assert_equal [[11, 0], [21, 1], nil, [12, 2]], buf
    assert_equal({
      fiber: 2,
      block: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_fiber_scheduler_thread_join
    thread = Thread.new do
      sleep 0.1
    end
    Fiber.schedule do
      thread.join
    end

    # No ops are issued, except for a NOP SQE used to wakeup the waiting thread.
    assert_equal 0, machine.total_op_count

    @scheduler.join
    assert_equal 1, machine.total_op_count
    assert_equal({
      fiber: 1,
      block: 1,
      unblock: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_fiber_scheduler_system
    skip if !@scheduler.respond_to?(:process_wait)

    buf = []
    Fiber.schedule do
      buf << system('sleep 0.01')
    end
    @scheduler.join
    assert_equal [true], buf
    assert_equal({
      fiber: 1,
      process_wait: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  ensure
    Process.wait(0, Process::WNOHANG) rescue nil
  end

  def test_fiber_scheduler_cmd
    skip if !@scheduler.respond_to?(:process_wait)

    buf = []
    Fiber.schedule do
      buf << `echo 'foo'`
    end
    assert_equal 1, machine.total_op_count
    @scheduler.join
    assert_equal ["foo\n"], buf
    assert_equal({
      fiber: 1,
      io_read: 2,
      process_wait: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  ensure
    Process.wait(0, Process::WNOHANG) rescue nil
  end

  def test_fiber_scheduler_popen
    skip if !@scheduler.respond_to?(:process_wait)

    buf = []
    Fiber.schedule do
      IO.popen('ruby', 'r+') do |pipe|
        buf << [11, machine.total_op_count]    
        pipe.puts 'puts "bar"'
        buf << [12, machine.total_op_count]    
        pipe.close_write
        buf << [13, pipe.gets.chomp, machine.total_op_count]
      end
    end
    assert_equal 1, machine.total_op_count
    @scheduler.join
    assert_equal [[11, 0], [12, 3], [13, "bar", 5]], buf
    assert_equal({
      fiber: 1,
      io_write: 2,
      io_read: 1,
      blocking_operation_wait: 1,
      process_wait: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  ensure
    Process.wait(0, Process::WNOHANG) rescue nil
  end

  def test_fiber_interrupt
    r, w = IO.pipe
    w << 'foo'

    exception = nil
    Fiber.schedule do
      r.read
    rescue Exception => e
      exception = e
    end
    assert_equal 1, machine.total_op_count
    machine.snooze
    Thread.new {
      r.close
    }
    @scheduler.join
    assert_kind_of IOError, exception
    assert_equal({
      fiber: 1,
      io_read: 2,
      fiber_interrupt: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  ensure
    r.close rescue nil
    w.close rescue nil
  end

  def test_address_resolve
    addrs = nil
    Fiber.schedule do
      addrs = Addrinfo.getaddrinfo("localhost", 80, Socket::AF_INET, :STREAM)
    end
    assert_equal 1, machine.total_op_count
    @scheduler.join
    assert_kind_of Array, addrs
    addr = addrs.first
    assert_kind_of Addrinfo, addr
    assert_includes ['127.0.0.1', '::1'], addr.ip_address
    assert_equal({
      fiber: 1,
      io_read: 2,
      blocking_operation_wait: 1,
      address_resolve: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_timeout_after
    res = nil
    Fiber.schedule do
      Timeout.timeout(0.05) do
        sleep 1
      end
      res = true
    rescue => e
      res = e
    end
    @scheduler.join
    assert_equal 3, machine.total_op_count
    assert_kind_of Timeout::Error, res
    assert_equal({
      fiber: 1,
      timeout_after: 1,
      kernel_sleep: 1,
      join: 1
    }, @scheduler.calls.map { it[:sym] }.tally)
  end

  def test_io_select
    r, w = IO.pipe
    buf = []

    Fiber.schedule do
      buf << IO.select([r], [], [])
      buf << IO.select([], [w], [])
    end
    @machine.snooze
    w << 'foo'
    @machine.snooze
    assert_equal [[[r], [], []]], buf
    @machine.snooze
    @scheduler.join
    assert_equal [[[r], [], []], [[], [w], []]], buf
  ensure
    r.close rescue nil
    w.close rescue nil
  end

  def test_blocking_operation_wait_single
    buf = []
    (1..10).each { |i|
      op = -> { i * 10}
      buf << @scheduler.blocking_operation_wait(op)
      sleep 0.01
      @machine.snooze
    }
    assert_equal (1..10).map { it * 10 }, buf

    buf = []
    (1..20).each { |i|
      op = -> { i * 10}
      Fiber.schedule do
        sleep 0.001
        buf << @scheduler.blocking_operation_wait(op)
        sleep 0.001
      end
    }
    @scheduler.join

    assert_equal (1..20).map { it * 10 }, buf.sort
  end
end
