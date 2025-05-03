# frozen_string_literal: true

require_relative 'helper'
require 'socket'

class AsyncOpTest < UMBaseTest
  def setup
    super
    @t0 = monotonic_clock
    @op = machine.prep_timeout(0.05)
  end

  def test_async_op_await
    assert_equal 1, machine.pending_count
    res = @op.await
    t1 = monotonic_clock
    assert_in_range 0.04..0.08, t1 - @t0
    assert_equal 0, machine.pending_count
    assert_equal (-ETIME), res
    assert_equal true, @op.done?
    assert_equal false, @op.cancelled?
  end

  def test_async_op_join
    assert_equal 1, machine.pending_count
    res = @op.join
    t1 = monotonic_clock
    assert_in_range 0.04..0.08, t1 - @t0
    assert_equal 0, machine.pending_count
    assert_equal (-ETIME), res
    assert_equal true, @op.done?
    assert_equal false, @op.cancelled?
  end

  def test_async_op_cancel
    machine.sleep(0.01)
    assert_equal 1, machine.pending_count
    @op.cancel
    assert_equal false, @op.done?

    machine.sleep(0.01)

    assert_equal 0, machine.pending_count
    assert_equal true, @op.done?
    assert_equal (-ECANCELED), @op.result
    assert_equal true, @op.cancelled?
  end

  def test_async_op_await_with_cancel
    machine.spin do
      @op.cancel
    end

    res = @op.await

    assert_equal 0, machine.pending_count
    assert_equal true, @op.done?
    assert_equal (-ECANCELED), res
    assert_equal true, @op.cancelled?
  end

  class TOError < RuntimeError; end

  def test_async_op_await_with_timeout
    e = nil

    begin
      machine.timeout(0.01, TOError) do
        @op.await
      end
    rescue => e
    end

    assert_equal 0, machine.pending_count
    assert_kind_of TOError, e
    assert_equal true, @op.done?
    assert_equal (-ECANCELED), @op.result
    assert_equal true, @op.cancelled?
  end

  def test_async_op_await_with_timeout2
    e = nil

    begin
      machine.timeout(0.1, TOError) do
        @op.await
      end
    rescue => e
    end

    # machine.timeout is cancelled async, so CQE is not yet reaped
    assert_equal 1, machine.pending_count
    assert_nil e
    assert_equal true, @op.done?
    assert_equal (-ETIME), @op.result
    assert_equal false, @op.cancelled?

    # wait for timeout cancellation
    machine.sleep(0.01)
    assert_equal 0, machine.pending_count
  end
end

class PrepTimeoutTest < UMBaseTest
  def test_prep_timeout
    op = machine.prep_timeout(0.03)
    assert_kind_of UM::AsyncOp, op
    assert_equal :timeout, op.kind

    assert_equal false, op.done?
    assert_nil op.result

    machine.sleep(0.05)

    assert_equal true, op.done?
    assert_equal (-ETIME), op.result
    assert_equal false, op.cancelled?
  end
end
