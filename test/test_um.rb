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
    machine.snooze

    c = CustomError.new
    machine.schedule(f, c)
    machine.snooze

    assert_equal [c], buf
  end

  # def test_interrupt
  #   cur = Fiber.current
  #   f = Fiber.new do
  #     p 1
  #     sleep 0.03
  #     p 2
  #     machine.interrupt(cur, 42)
  #   end
  #   machine.schedule(f, nil)

  #   t0 = monotonic_clock
  #   p 3
  #   res = machine.sleep(0.1)
  #   p 4
  #   t1 = monotonic_clock

  #   assert_equal 42, res
  #   assert_in_range 0.02..0.04, t1 - t0
  # end

  # def test_timeout
  #   buf = []
  #   t0 = monotonic_clock
  #   res = machine.timeout(0.5, 42) do
  #     buf << :sleep_pre
  #     machine.sleep(0.1)
  #     buf << :sleep_post
  #   end
  #   t1 = monotonic_clock
  #   assert_in_range 0.09..0.13, t1 - t0
  #   assert_equal 0.1, res
  #   assert_equal [:sleep_pre, :sleep_post], buf

  #   buf = []
  #   t0 = monotonic_clock
  #   res = machine.timeout(0.05, 42) do
  #     buf << :sleep_pre
  #     machine.sleep(0.1)
  #     buf << :sleep_post
  #   end
  #   t1 = monotonic_clock
  #   assert_in_range 0.04..0.06, t1 - t0
  #   assert_equal 42, res
  #   assert_equal [:sleep_pre], buf
  # end
end
