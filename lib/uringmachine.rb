# frozen_string_literal: true

require_relative './um_ext'
require_relative 'uringmachine/dns_resolver'

UM = UringMachine

class UringMachine
  @@fiber_map = {}

  def fiber_map
    @@fiber_map
  end

  class Terminate < Exception
  end

  def spin(value = nil, fiber_class = Fiber, &block)
    f = fiber_class.new do |resume_value|
      f.set_result block.(resume_value)
    rescue Exception => e
      f.set_result e
    ensure
      f.mark_as_done
      # cleanup
      @@fiber_map.delete(f)
      self.notify_done_listeners(f)
      # transfer control to other fibers
      self.yield
    end
    self.schedule(f, value)
    @@fiber_map[f] = true
    f
  end

  def join(*fibers)
    results = fibers.inject({}) { |h, f| h[f] = nil; h }
    queue = nil
    pending = nil
    fibers.each do |f|
      if f.done?
        results[f] = f.result
      else
        (pending ||= []) << f
        queue ||= UM::Queue.new
        f.add_done_listener(queue)
      end
    end
    return results.values if !pending

    while !pending.empty?
      f = self.shift(queue)
      pending.delete(f)
      results[f] = f.result
    end
    results.values
  end

  def resolve(hostname, type = :A)
    @resolver ||= DNSResolver.new(self)
    @resolver.resolve(hostname, type)
  end

  private

  def notify_done_listeners(fiber)
    listeners = fiber.done_listeners
    return if !listeners

    listeners.each { self.push(it, fiber) }
  end

  module FiberExtensions
    attr_reader :result, :done, :done_listeners

    def mark_as_done
      @done = true
    end

    def set_result(value)
      @result = value
    end

    def done?
      @done
    end

    def add_done_listener(queue)
      (@done_listeners ||= []) << queue
    end
  end

  class ::Fiber
    include UringMachine::FiberExtensions
  end
end
