# frozen_string_literal: true

require_relative './um_ext'

UM = UringMachine

class UringMachine
  def spin(value = nil, &block)
    Fiber.new do |resume_value| 
      block.(resume_value)
    rescue Exception => e
      STDERR.puts "Unhandled fiber exception: #{e.inspect}"
      STDERR.puts e.backtrace.join("\n")
      exit
    ensure
      # yield control
      self.yield
    end.tap { |f| schedule(f, value) }
  end
end
