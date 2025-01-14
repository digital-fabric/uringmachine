# frozen_string_literal: true

require_relative './um_ext'
require_relative 'uringmachine/dns_resolver'

UM = UringMachine

class UringMachine
  @@fiber_map = {}

  def fiber_map
    @@fiber_map
  end

  def spin(value = nil, fiber_class = Fiber, &block)
    f = fiber_class.new do |resume_value|
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
    f
  end

  def resolve(hostname, type = :A)
    @resolver ||= DNSResolver.new(self)
    @resolver.resolve(hostname, type)
  end

  def ssl_accept(fd, ssl_ctx)
    SSL::Connection.new(self, fd, ssl_ctx)
  end
end
