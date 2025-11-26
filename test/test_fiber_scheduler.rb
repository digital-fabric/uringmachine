# frozen_string_literal: true

require_relative 'helper'
require 'uringmachine/fiber_scheduler'

class FiberSchedulerTest < UMBaseTest
  def setup
    super
    @scheduler = UM::FiberScheduler.new(@machine)
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

  def test_fiber_scheduler_post_fork
    Fiber.schedule {}
    assert_equal 1, @scheduler.fiber_map.size
    
    machine_before = @scheduler.machine
    @scheduler.post_fork
    refute_equal machine_before, @scheduler.machine
    assert_equal 0, @scheduler.fiber_map.size
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
    assert_equal 2, @scheduler.fiber_map.size

    # close scheduler
    Fiber.set_scheduler nil
    GC.start
    assert_equal 0, @scheduler.fiber_map.size
  end

  def test_fiber_scheduler_basic_io
    i, o = IO.pipe
    buffer = []

    f1 = Fiber.schedule do
      sleep 0.001
      o.write 'foo'
      buffer << :f1
    end

    f2 = Fiber.schedule do
      sleep 0.002
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
  ensure
    i.close rescue nil
    o.close rescue nil
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
    assert_in_range 0.01..0.015, t1 - t0
  end

  def test_fiber_scheduler_process_wait
    child_pid = nil
    status = nil
    f1 = Fiber.schedule do
      child_pid = fork {
        Fiber.scheduler.post_fork
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
  ensure
    if child_pid
      Process.wait(child_pid) rescue nil
    end
  end
end
