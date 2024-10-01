# frozen_string_literal: true

require_relative 'helper'
require 'socket'

class SleepTest < IOURingBaseTest
  def test_sleep
    t0 = monotonic_clock
    res = @machine.sleep(0.1)
    t1 = monotonic_clock
    assert_in_range 0.09..0.13, t1 - t0
    assert_equal 0.1, res
  end
end

