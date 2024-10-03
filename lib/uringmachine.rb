# frozen_string_literal: true

require_relative './um_ext'

UM = UringMachine

class UringMachine
  def self.raise_exception(e)
    raise e
  end


  # def timeout(interval, timeout_value)
  #   cur = Fiber.current
  #   f = Fiber.new do
  #     sleep interval
  #     interrupt(cur, timeout_value)
  #   end
  #   schedule(cur, nil)
  #   f.transfer
  #   yield
  # ensure
  #   interrupt(f, nil)
  # end
end
