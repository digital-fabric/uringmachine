# frozen_string_literal: true

require_relative 'helper'
require 'socket'

class FiberSpinTest < UMBaseTest
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

  def test_spin_with_initial_value
    x = nil
    f = machine.spin(42) do |v|
      x = v
    end

    assert_kind_of Fiber, f
    assert_nil x

    machine.snooze
    assert_equal 42, x
  end

  class MyFiber < Fiber
  end

  def test_spin_with_custom_class
    f = machine.spin(nil, MyFiber) do
    end

    assert_kind_of MyFiber, f
  end
end

class FiberTerminateTest < UMBaseTest
  def test_terminate_fiber
    x = nil
    f = machine.spin do
      x = 1
      machine.sleep 0.01
    ensure
      x = 0
    end

    assert_nil x
    machine.snooze
    assert_equal 1, x

    machine.schedule(f, UM::Terminate.new)
    2.times { machine.snooze }

    assert_equal 0, x
  end
end

class FiberJoinTest < UMBaseTest
  def test_join
    q = UM::Queue.new
    x = nil

    f = machine.spin do
      x = 1
      machine.push q, machine.shift(q) + 1
      42
    ensure
      x = 0
    end

    assert_nil x
    machine.snooze
    assert_equal 1, x

    machine.spin do
      x = 2
      machine.push q, 2
    end

    res = machine.join(f)
    assert_equal 0, x
    assert_equal 3, machine.shift(q)
    assert_equal 42, res
  end

  def test_join_multiple
    f1 = machine.spin do
      :foo
    end

    f2 = machine.spin do
      machine.sleep(0.001)
      :bar
    end

    f3 = machine.spin do
      :baz
    end

    res = machine.join(f1, f2, f3)
    assert_equal [:foo, :bar, :baz], res
  end

  def test_join_cross_thread
    q = UM::Queue.new

    t2 = Thread.new do
      m2 = UM.new
      f = m2.spin do
        m2.push(q, f)
        m2.snooze
        :foo
      end
      m2.join(f)
    end

    f = machine.shift(q)
    assert_kind_of Fiber, f
    res = machine.join(f)
    assert_equal :foo, res
  ensure
    t2.join
  end

  def test_join_with_exception
    f = machine.spin do
      raise "Foobar"
    end

    e = machine.join(f)
    assert_kind_of RuntimeError, e
    assert_equal 'Foobar', e.message
  end
end

class WaitFibersTest < UMBaseTest
  def test_await_fibers
    q = UM::Queue.new
    x = nil

    f = machine.spin do
      x = 1
      machine.push q, machine.shift(q) + 1
      42
    ensure
      x = 0
    end

    assert_nil x
    machine.snooze
    assert_equal 1, x

    machine.spin do
      x = 2
      machine.push q, 2
    end

    res = machine.await_fibers([f])
    assert_equal 0, x
    assert_equal 3, machine.shift(q)
    assert_equal 1, res

    done = nil
    f = machine.spin { machine.snooze; done = true }
    res = machine.await_fibers(f)
    assert done
    assert_equal 1, res
  end

  def test_await_fibers_multiple
    f1 = machine.spin do
      :foo
    end

    f2 = machine.spin do
      machine.sleep(0.001)
      :bar
    end

    f3 = machine.spin do
      :baz
    end

    res = machine.await_fibers([f1, f2, f3])
    assert_equal 3, res
  end

  def test_await_fibers_cross_thread
    q = UM::Queue.new

    t2 = Thread.new do
      m2 = UM.new
      f = m2.spin do
        m2.push(q, f)
        m2.snooze
        :foo
      end
      m2.await_fibers(f)
    end

    f = machine.shift(q)
    assert_kind_of Fiber, f
    res = machine.await_fibers(f)
    assert_equal 1, res
  ensure
    t2.join
  end

  def test_await_fibers_with_exception
    f = machine.spin do
      raise "Foobar"
    end

    res = machine.await_fibers(f)
    assert_equal 1, res
  end

  def test_await_fibers_terminate
    f1 = machine.spin { machine.sleep(1) }
    f2 = machine.spin { machine.sleep(1) }
    done = false
    a = machine.spin do
      machine.await_fibers([f1, f2])
    rescue UM::Terminate
      done = true
    end

    machine.snooze
    machine.schedule(a, UM::Terminate.new)
    machine.join(a)
    assert_equal true, done
  end
end

class ScopeTest < UMBaseTest
  def test_scope
    skip("UM#scope not yet implemented")

    x1 = nil
    x2 = nil

    t0 = monotonic_clock
    machine.scope do
      machine.spin do
        x1 = 1
        machine.sleep 0.01
      ensure
        x1 = 0
      end

      machine.spin do
        x2 = 1
        machine.sleep 0.03
      ensure
        x2 = 0
      end
    end
    elapsed = monotonic_clock - t0
    assert_in_range 0.03..0.05, elapsed

    assert_equal 0, x1
    assert_equal 0, x2
  end
end

class FiberMailboxTest < UMBaseTest
  def test_fiber_mailbox
    m = Fiber.current.mailbox
    assert_kind_of UM::Queue, m

    m2 = Fiber.current.mailbox
    assert_equal m, m2
  end
end

class ThreadMachineTest < UMBaseTest
  def test_thread_machine
    m = Thread.current.machine
    assert_kind_of UM, m

    m2 = Thread.current.machine
    assert_equal m, m2
  end
end
