# frozen_string_literal: true

require_relative './um_ext'
require_relative 'uringmachine/dns_resolver'

UM = UringMachine

class UringMachine
  @@fiber_map = {}

  def spin(value = nil, &block)
    f = Fiber.new do |resume_value|
      block.(resume_value)
    rescue Exception => e
      STDERR.puts "Unhandled fiber exception: #{e.inspect}"
      STDERR.puts e.backtrace.join("\n")
      exit
    ensure
      @@fiber_map.delete(f)
      # yield control
      self.yield
      p :bad_bad_bad
    end
    schedule(f, value)
    @@fiber_map[f] = true
  end

  def resolve(hostname, type = :A)
    @resolver ||= DNSResolver.new(self)
    @resolver.resolve(hostname, type)
  end
end
