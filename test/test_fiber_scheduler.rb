# frozen_string_literal: true

require_relative 'helper'
require 'uringmachine/fiber_scheduler'

class FiberSchedulerTest < UMBaseTest
  def setup
    super
    @scheduler = UM::FiberScheduler.new(@machine)
    Fiber.set_scheduler @scheduler
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
      o.close
      buffer << :f2
    end

    f3 = Fiber.schedule do
      str = i.read
      buffer << str
    end

    @scheduler.join
    assert_equal [true] * 3, [f1, f2, f3].map(&:done?)
    assert_equal [:f1, :f2, 'foobar'], buffer
  end

  def test_fiber_scheduler_sleep
    t0 = monotonic_clock
    assert_equal 0, machine.pending_count
    f1 = Fiber.schedule do
      sleep(0.01)
    end
    f2 = Fiber.schedule do
      sleep(0.02)
    end
    assert_equal 2, machine.pending_count
    @scheduler.join
    t1 = monotonic_clock
    assert_in_range 0.01..0.025, t1 - t0
  end

  def test_fiber_scheduler_lock
    mutex = Mutex.new
    buffer = []
    t0 = monotonic_clock
    f1 = Fiber.schedule do
      10.times { sleep 0.001; buffer << it }
    end
    f2 = Fiber.schedule do
      mutex.synchronize { sleep(0.005) }
    end
    f3 = Fiber.schedule do
      mutex.synchronize { sleep(0.005) }
    end
    @scheduler.join
    t1 = monotonic_clock
    assert_in_range 0.01..0.015, t1 - t0
  end
end
