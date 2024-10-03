# frozen_string_literal: true

require_relative 'helper'
require 'socket'

class SleepTest < UMBaseTest
  def test_sleep
    t0 = monotonic_clock
    res = machine.sleep(0.1)
    t1 = monotonic_clock
    assert_in_range 0.09..0.13, t1 - t0
    assert_equal 0.1, res
  end
end

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
    # So we still have a pending operation.
    assert_equal 1, machine.pending_count
    machine.snooze

    # Snooze should have let the CQE be received
    assert_equal 0, machine.pending_count

    assert_equal [1, 2, 5], buf
    assert_kind_of TOError, e
  end

  def test_timeout_with_raising_block
    e = nil
    v = begin
      machine.timeout(0.01, TOError) do
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
    v = machine.timeout(0.01, TOError) { 42 }

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
